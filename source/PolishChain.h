#pragma once

#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <cmath>
#include <memory>

/**
    The "Audio Polish" signal chain.

    Signal flow:

        input gain
          -> tone (low shelf + high shelf, with a tilt fold-in)
          -> harmonic saturation (asymmetric tanh, 4x oversampled, DC-blocked)
          -> glue compression
          -> stereo width (mid/side, low end kept mono as width increases)
          -> output gain
          -> ceiling limiter (brick-wall safety / loudness)
          -> dry/wet mix (latency compensated)

    A single "Polish" macro pushes drive, glue, top-end "air" and width together
    so one knob takes a source from flat to finished, while the individual
    controls remain available for fine tuning.
*/
class PolishChain
{
public:
    struct Settings
    {
        float inputDb   = 0.0f;
        float polish    = 25.0f;   // 0..100 macro
        float lowDb     = 0.0f;
        float highDb    = 0.0f;
        float tiltDb    = 0.0f;
        float drive     = 20.0f;   // 0..100
        float glue      = 25.0f;   // 0..100
        float width     = 100.0f;  // 0..200 (100 == unchanged)
        float ceilingDb = -0.3f;
        float outputDb  = 0.0f;
        float mix       = 100.0f;  // 0..100 dry/wet
    };

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate  = spec.sampleRate;
        numChannels = static_cast<int> (spec.numChannels);

        // 2^2 == 4x oversampling around the saturation stage. Linear-phase FIR
        // for a clean, symmetric impulse; integer latency keeps dry alignment exact.
        oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
            spec.numChannels, 2,
            juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
            true, true);
        oversampler->initProcessing (spec.maximumBlockSize);
        oversamplingFactor = static_cast<double> (oversampler->getOversamplingFactor());
        latencySamples = static_cast<int> (std::round (oversampler->getLatencyInSamples()));

        inputGain.prepare (spec);
        outputGain.prepare (spec);
        makeupGain.prepare (spec);
        lowShelf.prepare (spec);
        highShelf.prepare (spec);
        compressor.prepare (spec);
        limiter.prepare (spec);

        inputGain.setRampDurationSeconds (0.02);
        outputGain.setRampDurationSeconds (0.02);
        makeupGain.setRampDurationSeconds (0.02);

        compressor.setAttack (15.0f);
        compressor.setRelease (140.0f);

        limiter.setRelease (50.0f);

        // Saturation drive is smoothed at the oversampled rate (it runs inside
        // the upsampled block); width/mix run at the base rate.
        driveSmoothed.reset (sampleRate * oversamplingFactor, 0.02);
        widthSmoothed.reset (sampleRate, 0.02);
        mixSmoothed.reset (sampleRate, 0.02);

        // ~120 Hz one-pole used to keep the low end of the side signal mono.
        bassMonoCoeff = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi
                                          * 120.0f / static_cast<float> (sampleRate));

        dryDelay.prepare (spec);
        dryDelay.setMaximumDelayInSamples (juce::jmax (1, latencySamples) + 8);
        dryDelay.setDelay (static_cast<float> (latencySamples));

        bypassDelay.prepare (spec);
        bypassDelay.setMaximumDelayInSamples (juce::jmax (1, latencySamples) + 8);
        bypassDelay.setDelay (static_cast<float> (latencySamples));

        dryBuffer.setSize (numChannels, static_cast<int> (spec.maximumBlockSize));

        reset();
    }

    void reset()
    {
        inputGain.reset();
        outputGain.reset();
        makeupGain.reset();
        lowShelf.reset();
        highShelf.reset();
        compressor.reset();
        limiter.reset();
        dryDelay.reset();
        bypassDelay.reset();

        if (oversampler != nullptr)
            oversampler->reset();

        for (auto& s : dcState) s = { 0.0f, 0.0f };
        sideLowState = 0.0f;

        outputLevel.store (0.0f);
        gainReduction.store (0.0f);
    }

    int getLatencySamples() const noexcept { return latencySamples; }

    void setSettings (const Settings& s)
    {
        const auto p = juce::jlimit (0.0f, 1.0f, s.polish * 0.01f);

        // ---- macro modulation -------------------------------------------------
        const auto driveAmt = juce::jlimit (0.0f, 1.0f, s.drive * 0.01f + p * 0.40f);
        const auto glueAmt  = juce::jlimit (0.0f, 1.0f, s.glue  * 0.01f + p * 0.50f);
        const auto airDb    = p * 4.0f;
        const auto widthAmt = juce::jlimit (0.0f, 2.0f, s.width * 0.01f + p * 0.10f);

        inputGain.setGainDecibels  (s.inputDb);
        outputGain.setGainDecibels (s.outputDb);

        const auto lowGain  = s.lowDb  - s.tiltDb;
        const auto highGain = s.highDb + s.tiltDb + airDb;

        updateShelf (lowShelf,  true,  150.0f,  lowGain);
        updateShelf (highShelf, false, 6000.0f, highGain);

        driveSmoothed.setTargetValue (driveAmt);

        const auto threshold = -glueAmt * 18.0f;
        const auto ratio     = 1.0f + glueAmt * 2.0f;
        compressor.setThreshold (threshold);
        compressor.setRatio (ratio);
        makeupGain.setGainDecibels (glueAmt * 4.0f);

        widthSmoothed.setTargetValue (widthAmt);
        mixSmoothed.setTargetValue (juce::jlimit (0.0f, 1.0f, s.mix * 0.01f));

        limiter.setThreshold (s.ceilingDb);
    }

    void process (juce::AudioBuffer<float>& buffer)
    {
        const auto numSamples = buffer.getNumSamples();
        const auto chans      = juce::jmin (numChannels, buffer.getNumChannels());

        // Snapshot the clean input and delay it to line up with the wet path.
        captureDelayedDry (buffer, chans, numSamples);

        juce::dsp::AudioBlock<float> block (buffer);
        juce::dsp::ProcessContextReplacing<float> ctx (block);

        inputGain.process (ctx);

        lowShelf.process (ctx);
        highShelf.process (ctx);

        applySaturation (buffer);

        const auto preComp = buffer.getMagnitude (0, numSamples);
        compressor.process (ctx);
        const auto postComp = buffer.getMagnitude (0, numSamples);
        updateGainReduction (preComp, postComp);

        makeupGain.process (ctx);

        applyWidth (buffer);

        outputGain.process (ctx);

        limiter.process (ctx);

        applyMix (buffer, chans, numSamples);

        outputLevel.store (buffer.getMagnitude (0, numSamples));
    }

    /** Passes audio through unprocessed, but keeps the reported latency so the
        host's delay compensation stays aligned when the plugin is bypassed. */
    void processBypassed (juce::AudioBuffer<float>& buffer)
    {
        const auto chans      = juce::jmin (numChannels, buffer.getNumChannels());
        const auto numSamples = buffer.getNumSamples();

        for (int ch = 0; ch < chans; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            for (int n = 0; n < numSamples; ++n)
            {
                bypassDelay.pushSample (ch, data[n]);
                data[n] = bypassDelay.popSample (ch);
            }
        }

        outputLevel.store (buffer.getMagnitude (0, numSamples));
        gainReduction.store (0.0f);
    }

    /** Most recent output peak magnitude (linear), for metering. */
    float getOutputLevel() const noexcept { return outputLevel.load(); }

    /** Most recent compressor gain reduction in dB (<= 0), for metering. */
    float getGainReduction() const noexcept { return gainReduction.load(); }

private:
    using Filter = juce::dsp::IIR::Filter<float>;
    using Coeffs = juce::dsp::IIR::Coefficients<float>;

    struct DCBlocker { float x1, y1; };

    void updateShelf (juce::dsp::ProcessorDuplicator<Filter, Coeffs>& shelf,
                      bool isLow, float freq, float gainDb)
    {
        const auto gainLinear = juce::Decibels::decibelsToGain (gainDb);
        auto coeffs = isLow ? Coeffs::makeLowShelf  (sampleRate, freq, 0.707f, gainLinear)
                            : Coeffs::makeHighShelf (sampleRate, freq, 0.707f, gainLinear);
        *shelf.state = *coeffs;
    }

    void captureDelayedDry (const juce::AudioBuffer<float>& buffer, int chans, int numSamples)
    {
        for (int ch = 0; ch < chans; ++ch)
        {
            const auto* in = buffer.getReadPointer (ch);
            auto* dry      = dryBuffer.getWritePointer (ch);
            for (int n = 0; n < numSamples; ++n)
            {
                dryDelay.pushSample (ch, in[n]);
                dry[n] = dryDelay.popSample (ch);
            }
        }
    }

    void applySaturation (juce::AudioBuffer<float>& buffer)
    {
        juce::dsp::AudioBlock<float> block (buffer);
        auto osBlock = oversampler->processSamplesUp (block);

        const auto osSamples = static_cast<int> (osBlock.getNumSamples());
        const auto osChans   = static_cast<int> (osBlock.getNumChannels());

        for (int n = 0; n < osSamples; ++n)
        {
            const auto k    = driveSmoothed.getNextValue();   // 0..1
            const auto g    = 1.0f + k * 3.0f;                // 1..4 pre-gain
            const auto norm = 1.0f / std::tanh (g);           // keep ±1 mapping
            const auto bias = 0.25f * k;                      // even-harmonic asymmetry

            for (int ch = 0; ch < osChans; ++ch)
            {
                auto* data = osBlock.getChannelPointer (static_cast<size_t> (ch));
                const auto x   = data[n];
                const auto asy = x + bias * x * x;            // adds even harmonics
                const auto sat = std::tanh (g * asy) * norm;
                data[n] = x * (1.0f - k) + sat * k;           // dry/wet blend
            }
        }

        oversampler->processSamplesDown (block);

        // The asymmetric shaping introduces a small DC offset; remove it.
        applyDCBlocker (buffer);
    }

    void applyDCBlocker (juce::AudioBuffer<float>& buffer)
    {
        constexpr float R = 0.9975f;
        const auto chans = juce::jmin (numChannels, buffer.getNumChannels());

        for (int ch = 0; ch < chans; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            auto& s    = dcState[(size_t) ch];

            for (int n = 0; n < buffer.getNumSamples(); ++n)
            {
                const auto x = data[n];
                const auto y = x - s.x1 + R * s.y1;
                s.x1 = x;
                s.y1 = y;
                data[n] = y;
            }
        }
    }

    void applyWidth (juce::AudioBuffer<float>& buffer)
    {
        if (buffer.getNumChannels() < 2)
        {
            for (int n = 0; n < buffer.getNumSamples(); ++n)
                widthSmoothed.getNextValue();
            return;
        }

        auto* left  = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);

        for (int n = 0; n < buffer.getNumSamples(); ++n)
        {
            const auto w        = widthSmoothed.getNextValue();
            const auto extraWide = juce::jlimit (0.0f, 1.0f, w - 1.0f);

            const auto mid  = 0.5f * (left[n] + right[n]);
            const auto side = 0.5f * (left[n] - right[n]);

            // Track the low band of the side signal and pull it back out in
            // proportion to how far past "normal" we are widening. At w <= 1
            // this is fully transparent; toward w = 2 the bass becomes mono.
            sideLowState += bassMonoCoeff * (side - sideLowState);
            const auto widened = (side - extraWide * sideLowState) * w;

            left[n]  = mid + widened;
            right[n] = mid - widened;
        }
    }

    void applyMix (juce::AudioBuffer<float>& buffer, int chans, int numSamples)
    {
        for (int n = 0; n < numSamples; ++n)
        {
            const auto wet = mixSmoothed.getNextValue();
            const auto dry = 1.0f - wet;

            for (int ch = 0; ch < chans; ++ch)
            {
                auto* data    = buffer.getWritePointer (ch);
                const auto* d = dryBuffer.getReadPointer (ch);
                data[n] = data[n] * wet + d[n] * dry;
            }
        }
    }

    void updateGainReduction (float preComp, float postComp)
    {
        if (preComp > 1.0e-6f && postComp > 1.0e-6f)
        {
            const auto gr = juce::Decibels::gainToDecibels (postComp / preComp);
            gainReduction.store (juce::jlimit (-24.0f, 0.0f, gr));
        }
        else
        {
            gainReduction.store (0.0f);
        }
    }

    double sampleRate = 44100.0;
    double oversamplingFactor = 4.0;
    int    numChannels = 2;
    int    latencySamples = 0;

    juce::dsp::Gain<float> inputGain, outputGain, makeupGain;
    juce::dsp::ProcessorDuplicator<Filter, Coeffs> lowShelf, highShelf;
    juce::dsp::Compressor<float> compressor;
    juce::dsp::Limiter<float> limiter;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;

    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> dryDelay { 256 };
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> bypassDelay { 256 };
    juce::AudioBuffer<float> dryBuffer;

    juce::SmoothedValue<float> driveSmoothed { 0.2f };
    juce::SmoothedValue<float> widthSmoothed { 1.0f };
    juce::SmoothedValue<float> mixSmoothed   { 1.0f };

    std::array<DCBlocker, 2> dcState { { { 0.0f, 0.0f }, { 0.0f, 0.0f } } };
    float bassMonoCoeff = 0.0f;
    float sideLowState  = 0.0f;

    std::atomic<float> outputLevel  { 0.0f };
    std::atomic<float> gainReduction { 0.0f };
};
