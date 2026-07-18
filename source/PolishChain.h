#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include "LoudnessMeter.h"

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
    bool  loudnessMatch = false; // gain-compensate the Bypass passthrough to match Polish's loudness
    bool  ditherOn      = false; // TPDF dither before final output
    int   ditherBits    = 16;    // target bit depth the dither noise is scaled for
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
          -> ceiling limiter (true-peak safe: runs inside a fixed 4x oversampled block)
          -> dry/wet mix (latency compensated)
          -> optional TPDF dither

    A single "Polish" macro pushes drive, glue, top-end "air" and width together
    so one knob takes a source from flat to finished, while the individual
    controls remain available for fine tuning.
*/
template <typename Sample>
class PolishChain
{
public:
    /** @param osExponent  oversampling factor as a power of two for the
                            saturation stage: 0 = off (1x), 1 = 2x, 2 = 4x, 3 = 8x. */
    void prepare (const juce::dsp::ProcessSpec& spec, int osExponent)
    {
        sampleRate  = spec.sampleRate;
        numChannels = static_cast<int> (spec.numChannels);

        // Linear-phase FIR for a clean, symmetric impulse; integer latency keeps
        // dry alignment exact. When osExponent is 0 the saturator runs at the
        // base rate with no oversampler and no added latency.
        if (osExponent > 0)
        {
            oversampler = std::make_unique<juce::dsp::Oversampling<Sample>> (
                spec.numChannels, static_cast<size_t> (osExponent),
                juce::dsp::Oversampling<Sample>::filterHalfBandFIREquiripple,
                true, true);
            oversampler->initProcessing (spec.maximumBlockSize);
            oversamplingFactor = static_cast<double> (oversampler->getOversamplingFactor());
            latencySamples = static_cast<int> (std::round (oversampler->getLatencyInSamples()));
        }
        else
        {
            oversampler.reset();
            oversamplingFactor = 1.0;
            latencySamples = 0;
        }

        // Fixed (always-on, not user-switchable) oversampling around the ceiling
        // limiter so it acts on the reconstructed waveform: brick-walling the
        // oversampled signal catches inter-sample ("true") peaks that a 1x
        // limiter would let through. IIR polyphase keeps its added latency
        // minimal, unlike the linear-phase FIR used for the Drive stage above.
        ceilingOversampler = std::make_unique<juce::dsp::Oversampling<Sample>> (
            spec.numChannels, 2,
            juce::dsp::Oversampling<Sample>::filterHalfBandPolyphaseIIR,
            true, false);
        ceilingOversampler->initProcessing (spec.maximumBlockSize);
        const auto ceilingFactor = static_cast<double> (ceilingOversampler->getOversamplingFactor());
        const auto ceilingLatencySamples = static_cast<int> (std::round (ceilingOversampler->getLatencyInSamples()));
        latencySamples += ceilingLatencySamples;

        inputGain.prepare (spec);
        outputGain.prepare (spec);
        makeupGain.prepare (spec);
        lowShelf.prepare (spec);
        highShelf.prepare (spec);
        compressor.prepare (spec);

        auto ceilingSpec = spec;
        ceilingSpec.sampleRate       = spec.sampleRate * ceilingFactor;
        ceilingSpec.maximumBlockSize = spec.maximumBlockSize * static_cast<juce::uint32> (ceilingFactor);
        limiter.prepare (ceilingSpec);

        inputGain.setRampDurationSeconds (0.02);
        outputGain.setRampDurationSeconds (0.02);
        makeupGain.setRampDurationSeconds (0.02);

        compressor.setAttack (15.0f);
        compressor.setRelease (140.0f);

        limiter.setRelease (50.0f);

        driveSmoothed.reset (sampleRate * oversamplingFactor, 0.02);
        widthSmoothed.reset (sampleRate, 0.02);
        mixSmoothed.reset (sampleRate, 0.02);
        abGainSmoothed.reset (sampleRate, 0.1);

        dryLoudness.prepare (sampleRate, numChannels);
        wetLoudness.prepare (sampleRate, numChannels);

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
        if (ceilingOversampler != nullptr)
            ceilingOversampler->reset();

        for (auto& s : dcState) s = { Sample (0), Sample (0) };
        sideLowState = Sample (0);

        dryLoudness.reset();
        wetLoudness.reset();
        abGainSmoothed.setCurrentAndTargetValue (Sample (0));

        outputLevel.store (0.0f);
        gainReduction.store (0.0f);
        truePeakDb.store (-100.0f);
        correlation.store (1.0f);
        abGainOffsetDb.store (0.0f);
    }

    /** Clears the long-term LUFS-Integrated history (e.g. "start of song"). */
    void resetLoudnessIntegration()
    {
        wetLoudness.resetIntegration();
        dryLoudness.resetIntegration();
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

        loudnessMatchEnabled = s.loudnessMatch;
        ditherEnabled        = s.ditherOn;
        ditherBitDepth       = s.ditherBits;
    }

    void process (juce::AudioBuffer<Sample>& buffer)
    {
        const auto numSamples = buffer.getNumSamples();
        const auto chans      = juce::jmin (numChannels, buffer.getNumChannels());

        captureDelayedDry (buffer, chans, numSamples);
        dryLoudness.process (dryBuffer.getArrayOfReadPointers(), chans, numSamples);

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

        applyCeiling (buffer);

        applyMix (buffer, chans, numSamples);

        wetLoudness.process (buffer.getArrayOfReadPointers(), chans, numSamples);
        updateAbGainOffset();
        updateCorrelation (buffer, chans, numSamples);

        if (ditherEnabled)
            applyDither (buffer, chans, numSamples, ditherBitDepth);

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

        dryLoudness.process (buffer.getArrayOfReadPointers(), chans, numSamples);
        updateCorrelation (buffer, chans, numSamples);

        // Loudness-matched compare: gain-compensate the passthrough by the last
        // measured Polish-vs-dry loudness difference, so flipping Bypass judges
        // the processing itself rather than which side happens to be louder.
        abGainSmoothed.setTargetValue (loudnessMatchEnabled
                                            ? static_cast<Sample> (abGainOffsetDb.load())
                                            : Sample (0));
        for (int n = 0; n < numSamples; ++n)
        {
            const auto g = static_cast<Sample> (juce::Decibels::decibelsToGain (
                               static_cast<float> (abGainSmoothed.getNextValue())));
            for (int ch = 0; ch < chans; ++ch)
                buffer.getWritePointer (ch)[n] *= g;
        }

        if (ditherEnabled)
            applyDither (buffer, chans, numSamples, ditherBitDepth);

        outputLevel.store (static_cast<float> (buffer.getMagnitude (0, numSamples)));
        gainReduction.store (0.0f);
    }

    /** Most recent output peak magnitude (linear), for metering. */
    float getOutputLevel() const noexcept { return outputLevel.load(); }

    /** Most recent compressor gain reduction in dB (<= 0), for metering. */
    float getGainReduction() const noexcept { return gainReduction.load(); }

    /** True-peak (oversampled, post-limiter) reading in dBTP, held during Bypass. */
    float getTruePeakDb() const noexcept { return truePeakDb.load(); }

    /** Stereo phase correlation of the final output, -1 (out of phase) .. +1 (mono-safe). */
    float getCorrelation() const noexcept { return correlation.load(); }

    float getLufsMomentary()  const noexcept { return wetLoudness.getMomentary(); }
    float getLufsShortTerm()  const noexcept { return wetLoudness.getShortTerm(); }
    float getLufsIntegrated() const noexcept { return wetLoudness.getIntegrated(); }

    /** Current Bypass loudness-match gain offset in dB, for display. */
    float getAbGainOffsetDb() const noexcept { return abGainOffsetDb.load(); }

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

        if (oversampler != nullptr)
        {
            auto osBlock = oversampler->processSamplesUp (block);
            shapeBlock (osBlock);
            oversampler->processSamplesDown (block);
        }
        else
        {
            shapeBlock (block);
        }

        applyDCBlocker (buffer);
    }

    void shapeBlock (juce::dsp::AudioBlock<Sample>& b)
    {
        const auto numSamples = static_cast<int> (b.getNumSamples());
        const auto numChans   = static_cast<int> (b.getNumChannels());

        for (int n = 0; n < numSamples; ++n)
        {
            const auto k    = driveSmoothed.getNextValue();          // 0..1
            const auto g    = static_cast<Sample> (1) + k * static_cast<Sample> (3); // 1..4
            const auto norm = static_cast<Sample> (1) / std::tanh (g);
            const auto bias = static_cast<Sample> (0.25) * k;        // even-harmonic asymmetry

            for (int ch = 0; ch < numChans; ++ch)
            {
                auto* data = b.getChannelPointer (static_cast<size_t> (ch));
                const auto x   = data[n];
                const auto asy = x + bias * x * x;                   // adds even harmonics
                const auto sat = std::tanh (g * asy) * norm;
                data[n] = x * (static_cast<Sample> (1) - k) + sat * k; // dry/wet blend
            }
        }
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

    /** Runs the ceiling limiter inside the fixed 4x oversampled block so the
        brick-wall threshold is enforced against the reconstructed waveform,
        not just the sample peaks -- i.e. it's true-peak safe. Also captures
        the post-limit true-peak reading used for metering. */
    void applyCeiling (juce::AudioBuffer<Sample>& buffer)
    {
        juce::dsp::AudioBlock<Sample> block (buffer);
        auto osBlock = ceilingOversampler->processSamplesUp (block);

        juce::dsp::ProcessContextReplacing<Sample> osCtx (osBlock);
        limiter.process (osCtx);

        ceilingOversampler->processSamplesDown (block);

        Sample peak = Sample (0);
        const auto n = osBlock.getNumSamples();
        const auto c = osBlock.getNumChannels();
        for (size_t ch = 0; ch < c; ++ch)
        {
            const auto* data = osBlock.getChannelPointer (ch);
            for (size_t i = 0; i < n; ++i)
                peak = juce::jmax (peak, std::abs (data[i]));
        }
        truePeakDb.store (juce::Decibels::gainToDecibels (static_cast<float> (peak), -100.0f));
    }

    void updateAbGainOffset()
    {
        const auto dryL = dryLoudness.getShortTerm();
        const auto wetL = wetLoudness.getShortTerm();

        // Ignore near-silence so a quiet passage doesn't swing the offset wildly.
        if (dryL > -60.0f && wetL > -60.0f)
            abGainOffsetDb.store (juce::jlimit (-24.0f, 24.0f, dryL - wetL));
    }

    void updateCorrelation (const juce::AudioBuffer<Sample>& buffer, int chans, int numSamples)
    {
        if (chans < 2)
        {
            correlation.store (1.0f);
            return;
        }

        const auto* l = buffer.getReadPointer (0);
        const auto* r = buffer.getReadPointer (1);

        double sumLR = 0.0, sumLL = 0.0, sumRR = 0.0;
        for (int n = 0; n < numSamples; ++n)
        {
            const auto lv = static_cast<double> (l[n]);
            const auto rv = static_cast<double> (r[n]);
            sumLR += lv * rv;
            sumLL += lv * lv;
            sumRR += rv * rv;
        }

        const auto denom = std::sqrt (sumLL * sumRR);
        const auto raw   = denom > 1.0e-9 ? static_cast<float> (juce::jlimit (-1.0, 1.0, sumLR / denom)) : 1.0f;

        // Smoothed for a readable meter rather than a per-block flicker.
        correlation.store (correlation.load() * 0.7f + raw * 0.3f);
    }

    void applyDither (juce::AudioBuffer<Sample>& buffer, int chans, int numSamples, int bitDepth)
    {
        const auto lsb = static_cast<Sample> (std::pow (2.0, -(bitDepth - 1)));

        for (int ch = 0; ch < chans; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            auto& rng  = ditherRng[(size_t) juce::jmin (ch, 1)];

            for (int n = 0; n < numSamples; ++n)
            {
                // Triangular PDF: sum of two independent uniform draws, range (-1, 1) LSB.
                const auto d = rng.nextFloat() - rng.nextFloat();
                data[n] += static_cast<Sample> (d) * lsb;
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
    std::unique_ptr<juce::dsp::Oversampling<Sample>> ceilingOversampler;

    juce::dsp::DelayLine<Sample, juce::dsp::DelayLineInterpolationTypes::Linear> dryDelay { 256 };
    juce::dsp::DelayLine<Sample, juce::dsp::DelayLineInterpolationTypes::Linear> bypassDelay { 256 };
    juce::AudioBuffer<Sample> dryBuffer;

    juce::SmoothedValue<Sample> driveSmoothed { static_cast<Sample> (0.2) };
    juce::SmoothedValue<Sample> widthSmoothed { static_cast<Sample> (1) };
    juce::SmoothedValue<Sample> mixSmoothed   { static_cast<Sample> (1) };
    juce::SmoothedValue<Sample> abGainSmoothed { Sample (0) };

    std::array<DCBlocker, 2> dcState { { { Sample (0), Sample (0) }, { Sample (0), Sample (0) } } };
    Sample bassMonoCoeff = Sample (0);
    Sample sideLowState  = Sample (0);

    LoudnessMeter<Sample> dryLoudness, wetLoudness;
    std::array<juce::Random, 2> ditherRng;

    bool loudnessMatchEnabled = false;
    bool ditherEnabled        = false;
    int  ditherBitDepth       = 16;

    std::atomic<float> outputLevel    { 0.0f };
    std::atomic<float> gainReduction  { 0.0f };
    std::atomic<float> truePeakDb     { -100.0f };
    std::atomic<float> correlation    { 1.0f };
    std::atomic<float> abGainOffsetDb { 0.0f };
};
