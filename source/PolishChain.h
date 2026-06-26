#pragma once

#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <cmath>

/**
    The "Audio Polish" signal chain.

    Signal flow:

        input gain
          -> tone (low shelf + high shelf, with a tilt fold-in)
          -> harmonic saturation (tanh soft-clip, dry/wet blended)
          -> glue compression
          -> stereo width (mid/side)
          -> output gain
          -> ceiling limiter (brick-wall safety / loudness)

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
    };

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;

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

        driveSmoothed.reset (sampleRate, 0.02);
        widthSmoothed.reset (sampleRate, 0.02);

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
        outputLevel.store (0.0f);
    }

    void setSettings (const Settings& s)
    {
        const auto p = juce::jlimit (0.0f, 1.0f, s.polish * 0.01f);

        // ---- macro modulation -------------------------------------------------
        // The Polish macro nudges the "character" stages without touching the
        // user's tonal balance choices too aggressively.
        const auto driveAmt = juce::jlimit (0.0f, 1.0f, s.drive * 0.01f + p * 0.40f);
        const auto glueAmt  = juce::jlimit (0.0f, 1.0f, s.glue  * 0.01f + p * 0.50f);
        const auto airDb    = p * 4.0f;                 // gentle top-end sheen
        const auto widthAmt = juce::jlimit (0.0f, 2.0f, s.width * 0.01f + p * 0.10f);

        // ---- gains ------------------------------------------------------------
        inputGain.setGainDecibels  (s.inputDb);
        outputGain.setGainDecibels (s.outputDb);

        // ---- tone: tilt folds opposite gains into the two shelves -------------
        const auto lowGain  = s.lowDb  - s.tiltDb;
        const auto highGain = s.highDb + s.tiltDb + airDb;

        updateShelf (lowShelf,  /*isLow*/ true,  150.0f, lowGain);
        updateShelf (highShelf, /*isLow*/ false, 6000.0f, highGain);

        // ---- saturation -------------------------------------------------------
        driveSmoothed.setTargetValue (driveAmt);

        // ---- glue compression (gentle, program-dependent) ---------------------
        const auto threshold = -glueAmt * 18.0f;        // 0 .. -18 dB
        const auto ratio     = 1.0f + glueAmt * 2.0f;   // 1:1 .. 3:1
        compressor.setThreshold (threshold);
        compressor.setRatio (ratio);
        makeupGain.setGainDecibels (glueAmt * 4.0f);

        // ---- width ------------------------------------------------------------
        widthSmoothed.setTargetValue (widthAmt);

        // ---- ceiling ----------------------------------------------------------
        limiter.setThreshold (s.ceilingDb);
    }

    void process (juce::AudioBuffer<float>& buffer)
    {
        juce::dsp::AudioBlock<float> block (buffer);
        juce::dsp::ProcessContextReplacing<float> ctx (block);

        inputGain.process (ctx);

        lowShelf.process (ctx);
        highShelf.process (ctx);

        applySaturation (buffer);

        compressor.process (ctx);
        makeupGain.process (ctx);

        applyWidth (buffer);

        outputGain.process (ctx);

        limiter.process (ctx);

        outputLevel.store (buffer.getMagnitude (0, buffer.getNumSamples()));
    }

    /** Most recent output peak magnitude (linear), for metering. */
    float getOutputLevel() const noexcept { return outputLevel.load(); }

private:
    using Filter = juce::dsp::IIR::Filter<float>;
    using Coeffs = juce::dsp::IIR::Coefficients<float>;

    void updateShelf (juce::dsp::ProcessorDuplicator<Filter, Coeffs>& shelf,
                      bool isLow, float freq, float gainDb)
    {
        const auto gainLinear = juce::Decibels::decibelsToGain (gainDb);
        auto coeffs = isLow ? Coeffs::makeLowShelf  (sampleRate, freq, 0.707f, gainLinear)
                            : Coeffs::makeHighShelf (sampleRate, freq, 0.707f, gainLinear);
        *shelf.state = *coeffs;
    }

    void applySaturation (juce::AudioBuffer<float>& buffer)
    {
        const auto numSamples  = buffer.getNumSamples();
        const auto numChannels = buffer.getNumChannels();

        for (int n = 0; n < numSamples; ++n)
        {
            const auto k = driveSmoothed.getNextValue();      // 0..1
            const auto g = 1.0f + k * 3.0f;                   // 1..4 pre-gain
            const auto norm = 1.0f / std::tanh (g);           // keep ±1 mapping

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* data = buffer.getWritePointer (ch);
                const auto x = data[n];
                const auto sat = std::tanh (g * x) * norm;
                data[n] = x * (1.0f - k) + sat * k;           // dry/wet blend
            }
        }
    }

    void applyWidth (juce::AudioBuffer<float>& buffer)
    {
        if (buffer.getNumChannels() < 2)
        {
            // keep the smoother in sync even when we can't apply width
            for (int n = 0; n < buffer.getNumSamples(); ++n)
                widthSmoothed.getNextValue();
            return;
        }

        auto* left  = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);

        for (int n = 0; n < buffer.getNumSamples(); ++n)
        {
            const auto w   = widthSmoothed.getNextValue();
            const auto mid  = 0.5f * (left[n] + right[n]);
            const auto side = 0.5f * (left[n] - right[n]) * w;
            left[n]  = mid + side;
            right[n] = mid - side;
        }
    }

    double sampleRate = 44100.0;

    juce::dsp::Gain<float> inputGain, outputGain, makeupGain;
    juce::dsp::ProcessorDuplicator<Filter, Coeffs> lowShelf, highShelf;
    juce::dsp::Compressor<float> compressor;
    juce::dsp::Limiter<float> limiter;

    juce::SmoothedValue<float> driveSmoothed { 0.2f };
    juce::SmoothedValue<float> widthSmoothed { 1.0f };

    std::atomic<float> outputLevel { 0.0f };
};
