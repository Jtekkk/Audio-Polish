#pragma once

#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <cmath>
#include <memory>

/**
    Parameter snapshot handed to the chain each block. Values mirror the
    user-facing parameters and are always float regardless of processing
    precision; the chain casts them to its sample type internally.
*/
struct PolishSettings
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

/**
    The "Audio Polish" signal chain, templated on the sample type so it can run
    in either 32-bit (float) or 64-bit (double) precision.

    Signal flow:

        input gain
          -> tone (low shelf + high shelf, with a tilt fold-in)
          -> harmonic saturation (asymmetric tanh, 8x oversampled, DC-blocked)
          -> glue compression
          -> stereo width (mid/side, low end kept mono as width increases)
          -> output gain
          -> ceiling limiter (brick-wall safety / loudness)
          -> dry/wet mix (latency compensated)

    A single "Polish" macro pushes drive, glue, top-end "air" and width together
    so one knob takes a source from flat to finished, while the individual
    controls remain available for fine tuning.
*/
template <typename Sample>
class PolishChain
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate  = spec.sampleRate;
        numChannels = static_cast<int> (spec.numChannels);

        // 2^3 == 8x oversampling around the saturation stage. Linear-phase FIR
        // for a clean, symmetric impulse; integer latency keeps dry alignment exact.
        oversampler = std::make_unique<juce::dsp::Oversampling<Sample>> (
            spec.numChannels, 3,
            juce::dsp::Oversampling<Sample>::filterHalfBandFIREquiripple,
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

        driveSmoothed.reset (sampleRate * oversamplingFactor, 0.02);
        widthSmoothed.reset (sampleRate, 0.02);
        mixSmoothed.reset (sampleRate, 0.02);

        bassMonoCoeff = static_cast<Sample> (1.0) - std::exp (static_cast<Sample> (-2.0)
                            * juce::MathConstants<Sample>::pi
                            * static_cast<Sample> (120.0) / static_cast<Sample> (sampleRate));

        dryDelay.prepare (spec);
        dryDelay.setMaximumDelayInSamples (juce::jmax (1, latencySamples) + 8);
        dryDelay.setDelay (static_cast<Sample> (latencySamples));

        bypassDelay.prepare (spec);
        bypassDelay.setMaximumDelayInSamples (juce::jmax (1, latencySamples) + 8);
        bypassDelay.setDelay (static_cast<Sample> (latencySamples));

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

        for (auto& s : dcState) s = { Sample (0), Sample (0) };
        sideLowState = Sample (0);

        outputLevel.store (0.0f);
        gainReduction.store (0.0f);
    }

    int getLatencySamples() const noexcept { return latencySamples; }

    void setSettings (const PolishSettings& s)
    {
        const auto p = juce::jlimit (0.0f, 1.0f, s.polish * 0.01f);

        // ---- macro modulation -------------------------------------------------
        const auto driveAmt = juce::jlimit (0.0f, 1.0f, s.drive * 0.01f + p * 0.40f);
        const auto glueAmt  = juce::jlimit (0.0f, 1.0f, s.glue  * 0.01f + p * 0.50f);
        const auto airDb    = p * 4.0f;
        const auto widthAmt = juce::jlimit (0.0f, 2.0f, s.width * 0.01f + p * 0.10f);

        inputGain.setGainDecibels  (static_cast<Sample> (s.inputDb));
        outputGain.setGainDecibels (static_cast<Sample> (s.outputDb));

        const auto lowGain  = s.lowDb  - s.tiltDb;
        const auto highGain = s.highDb + s.tiltDb + airDb;

        updateShelf (lowShelf,  true,  150.0f,  lowGain);
        updateShelf (highShelf, false, 6000.0f, highGain);

        driveSmoothed.setTargetValue (static_cast<Sample> (driveAmt));

        const auto threshold = -glueAmt * 18.0f;
        const auto ratio     = 1.0f + glueAmt * 2.0f;
        compressor.setThreshold (static_cast<Sample> (threshold));
        compressor.setRatio (static_cast<Sample> (ratio));
        makeupGain.setGainDecibels (static_cast<Sample> (glueAmt * 4.0f));

        widthSmoothed.setTargetValue (static_cast<Sample> (widthAmt));
        mixSmoothed.setTargetValue (static_cast<Sample> (juce::jlimit (0.0f, 1.0f, s.mix * 0.01f)));

        limiter.setThreshold (static_cast<Sample> (s.ceilingDb));
    }

    void process (juce::AudioBuffer<Sample>& buffer)
    {
        const auto numSamples = buffer.getNumSamples();
        const auto chans      = juce::jmin (numChannels, buffer.getNumChannels());

        captureDelayedDry (buffer, chans, numSamples);

        juce::dsp::AudioBlock<Sample> block (buffer);
        juce::dsp::ProcessContextReplacing<Sample> ctx (block);

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

        outputLevel.store (static_cast<float> (buffer.getMagnitude (0, numSamples)));
    }

    /** Passes audio through unprocessed, but keeps the reported latency so the
        host's delay compensation stays aligned when the plugin is bypassed. */
    void processBypassed (juce::AudioBuffer<Sample>& buffer)
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

        outputLevel.store (static_cast<float> (buffer.getMagnitude (0, numSamples)));
        gainReduction.store (0.0f);
    }

    /** Most recent output peak magnitude (linear), for metering. */
    float getOutputLevel() const noexcept { return outputLevel.load(); }

    /** Most recent compressor gain reduction in dB (<= 0), for metering. */
    float getGainReduction() const noexcept { return gainReduction.load(); }

private:
    using Filter = juce::dsp::IIR::Filter<Sample>;
    using Coeffs = juce::dsp::IIR::Coefficients<Sample>;

    struct DCBlocker { Sample x1, y1; };

    void updateShelf (juce::dsp::ProcessorDuplicator<Filter, Coeffs>& shelf,
                      bool isLow, float freq, float gainDb)
    {
        const auto gainLinear = juce::Decibels::decibelsToGain (static_cast<Sample> (gainDb));
        const auto f = static_cast<Sample> (freq);
        const auto q = static_cast<Sample> (0.707);
        auto coeffs = isLow ? Coeffs::makeLowShelf  (sampleRate, f, q, gainLinear)
                            : Coeffs::makeHighShelf (sampleRate, f, q, gainLinear);
        *shelf.state = *coeffs;
    }

    void captureDelayedDry (const juce::AudioBuffer<Sample>& buffer, int chans, int numSamples)
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

    void applySaturation (juce::AudioBuffer<Sample>& buffer)
    {
        juce::dsp::AudioBlock<Sample> block (buffer);
        auto osBlock = oversampler->processSamplesUp (block);

        const auto osSamples = static_cast<int> (osBlock.getNumSamples());
        const auto osChans   = static_cast<int> (osBlock.getNumChannels());

        for (int n = 0; n < osSamples; ++n)
        {
            const auto k    = driveSmoothed.getNextValue();          // 0..1
            const auto g    = static_cast<Sample> (1) + k * static_cast<Sample> (3); // 1..4
            const auto norm = static_cast<Sample> (1) / std::tanh (g);
            const auto bias = static_cast<Sample> (0.25) * k;        // even-harmonic asymmetry

            for (int ch = 0; ch < osChans; ++ch)
            {
                auto* data = osBlock.getChannelPointer (static_cast<size_t> (ch));
                const auto x   = data[n];
                const auto asy = x + bias * x * x;                   // adds even harmonics
                const auto sat = std::tanh (g * asy) * norm;
                data[n] = x * (static_cast<Sample> (1) - k) + sat * k; // dry/wet blend
            }
        }

        oversampler->processSamplesDown (block);

        applyDCBlocker (buffer);
    }

    void applyDCBlocker (juce::AudioBuffer<Sample>& buffer)
    {
        const auto R = static_cast<Sample> (0.9975);
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

    void applyWidth (juce::AudioBuffer<Sample>& buffer)
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
            const auto w         = widthSmoothed.getNextValue();
            const auto extraWide = juce::jlimit (Sample (0), Sample (1), w - static_cast<Sample> (1));

            const auto mid  = static_cast<Sample> (0.5) * (left[n] + right[n]);
            const auto side = static_cast<Sample> (0.5) * (left[n] - right[n]);

            sideLowState += bassMonoCoeff * (side - sideLowState);
            const auto widened = (side - extraWide * sideLowState) * w;

            left[n]  = mid + widened;
            right[n] = mid - widened;
        }
    }

    void applyMix (juce::AudioBuffer<Sample>& buffer, int chans, int numSamples)
    {
        for (int n = 0; n < numSamples; ++n)
        {
            const auto wet = mixSmoothed.getNextValue();
            const auto dry = static_cast<Sample> (1) - wet;

            for (int ch = 0; ch < chans; ++ch)
            {
                auto* data    = buffer.getWritePointer (ch);
                const auto* d = dryBuffer.getReadPointer (ch);
                data[n] = data[n] * wet + d[n] * dry;
            }
        }
    }

    void updateGainReduction (Sample preComp, Sample postComp)
    {
        if (preComp > static_cast<Sample> (1.0e-6) && postComp > static_cast<Sample> (1.0e-6))
        {
            const auto gr = juce::Decibels::gainToDecibels (postComp / preComp);
            gainReduction.store (juce::jlimit (-24.0f, 0.0f, static_cast<float> (gr)));
        }
        else
        {
            gainReduction.store (0.0f);
        }
    }

    double sampleRate = 44100.0;
    double oversamplingFactor = 8.0;
    int    numChannels = 2;
    int    latencySamples = 0;

    juce::dsp::Gain<Sample> inputGain, outputGain, makeupGain;
    juce::dsp::ProcessorDuplicator<Filter, Coeffs> lowShelf, highShelf;
    juce::dsp::Compressor<Sample> compressor;
    juce::dsp::Limiter<Sample> limiter;
    std::unique_ptr<juce::dsp::Oversampling<Sample>> oversampler;

    juce::dsp::DelayLine<Sample, juce::dsp::DelayLineInterpolationTypes::Linear> dryDelay { 256 };
    juce::dsp::DelayLine<Sample, juce::dsp::DelayLineInterpolationTypes::Linear> bypassDelay { 256 };
    juce::AudioBuffer<Sample> dryBuffer;

    juce::SmoothedValue<Sample> driveSmoothed { static_cast<Sample> (0.2) };
    juce::SmoothedValue<Sample> widthSmoothed { static_cast<Sample> (1) };
    juce::SmoothedValue<Sample> mixSmoothed   { static_cast<Sample> (1) };

    std::array<DCBlocker, 2> dcState { { { Sample (0), Sample (0) }, { Sample (0), Sample (0) } } };
    Sample bassMonoCoeff = Sample (0);
    Sample sideLowState  = Sample (0);

    std::atomic<float> outputLevel   { 0.0f };
    std::atomic<float> gainReduction { 0.0f };
};
