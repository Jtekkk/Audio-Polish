#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

/**
    Practical ITU-R BS.1770 / EBU R128 -style K-weighted loudness meter:
    Momentary (400 ms), Short-term (3 s) and gated Integrated loudness.

    This is a mastering-plugin metering aid, not a certified compliance
    meter -- it implements the standard K-weighting filter and the two-stage
    (absolute + relative) gating algorithm, but hasn't been calibration-tested
    against a reference suite.

    Channels are summed with equal (1.0) weighting, which is correct for the
    plain L/R layout this plugin supports (BS.1770 only up-weights surround
    channels, which don't apply here).
*/
template <typename Sample>
class LoudnessMeter
{
public:
    void prepare (double sampleRateIn, int numChannelsIn)
    {
        sampleRate  = sampleRateIn;
        numChannels = juce::jlimit (1, 2, numChannelsIn); // fixed 2-channel filter storage

        preCoeffs = makePreFilterCoeffs (sampleRate);
        rlbCoeffs = makeRlbCoeffs (sampleRate);

        for (auto& f : preFilter)  f.coefficients = preCoeffs;
        for (auto& f : rlbFilter) f.coefficients = rlbCoeffs;

        blockSamples = juce::jmax (1, (int) std::round (sampleRate * 0.1)); // 100 ms gating blocks

        reset();
        resetIntegration();
    }

    void reset()
    {
        for (auto& f : preFilter)  f.reset();
        for (auto& f : rlbFilter) f.reset();

        samplesInBlock  = 0;
        blockSumSquares = 0.0;

        momentaryBlocks.clear();
        shortTermBlocks.clear();

        momentary.store (-100.0f);
        shortTerm.store (-100.0f);
    }

    /** Clears the long-term integration history (e.g. "start of song"). */
    void resetIntegration()
    {
        gatingBlocks.clear();
        integrated.store (-100.0f);
    }

    /** Call once per processed block, on the audio thread. */
    void process (const Sample* const* channelData, int numChans, int numSamples)
    {
        const auto chans = juce::jmin (numChannels, numChans);
        if (chans <= 0 || numSamples <= 0)
            return;

        for (int n = 0; n < numSamples; ++n)
        {
            double sumSq = 0.0;
            for (int ch = 0; ch < chans; ++ch)
            {
                auto y = preFilter[(size_t) ch].processSample (channelData[ch][n]);
                y = rlbFilter[(size_t) ch].processSample (y);
                const auto yd = static_cast<double> (y);
                sumSq += yd * yd;
            }

            blockSumSquares += sumSq;
            channelsAccumulated = chans;

            if (++samplesInBlock >= blockSamples)
                finishBlock();
        }
    }

    /** Most recent readings, in LUFS (safe to call from any thread). */
    float getMomentary()  const noexcept { return momentary.load(); }
    float getShortTerm()  const noexcept { return shortTerm.load(); }
    float getIntegrated() const noexcept { return integrated.load(); }

private:
    void finishBlock()
    {
        const auto meanSq = blockSumSquares / (double) (samplesInBlock * juce::jmax (1, channelsAccumulated));
        blockSumSquares = 0.0;
        samplesInBlock  = 0;

        momentaryBlocks.push_back (meanSq);
        if (momentaryBlocks.size() > 4)
            momentaryBlocks.erase (momentaryBlocks.begin());

        shortTermBlocks.push_back (meanSq);
        if (shortTermBlocks.size() > 30)
            shortTermBlocks.erase (shortTermBlocks.begin());

        // Absolute gate (-70 LUFS) keeps near-silence out of the long-term stats.
        if (meanSq > absoluteGateMeanSquare)
            gatingBlocks.push_back (meanSq);

        momentary.store (loudnessOfMean (momentaryBlocks));
        shortTerm.store (loudnessOfMean (shortTermBlocks));
        integrated.store (computeIntegrated());
    }

    static float loudnessOfMean (const std::vector<double>& blocks)
    {
        if (blocks.empty())
            return -100.0f;

        double sum = 0.0;
        for (auto v : blocks)
            sum += v;

        return meanSquareToLufs (sum / (double) blocks.size());
    }

    float computeIntegrated() const
    {
        if (gatingBlocks.empty())
            return -100.0f;

        double sum = 0.0;
        for (auto v : gatingBlocks)
            sum += v;
        const auto ungatedMean = sum / (double) gatingBlocks.size();

        // Relative gate: -10 LU below the ungated mean == meanSquare / 10.
        const auto relativeGateMeanSquare = ungatedMean * 0.1;

        double gatedSum = 0.0;
        int gatedCount = 0;
        for (auto v : gatingBlocks)
        {
            if (v > relativeGateMeanSquare)
            {
                gatedSum += v;
                ++gatedCount;
            }
        }

        if (gatedCount == 0)
            return meanSquareToLufs (ungatedMean);

        return meanSquareToLufs (gatedSum / (double) gatedCount);
    }

    static float meanSquareToLufs (double meanSq)
    {
        if (meanSq <= 1.0e-12)
            return -100.0f;
        return static_cast<float> (-0.691 + 10.0 * std::log10 (meanSq));
    }

    // ---- BS.1770 K-weighting filter design (fixed constants from the spec) ----
    static typename juce::dsp::IIR::Coefficients<Sample>::Ptr makePreFilterCoeffs (double fs)
    {
        const double f0 = 1681.9744509555319;
        const double G  = 3.99984385397;
        const double Q  = 0.7071752369554193;

        const double K  = std::tan (juce::MathConstants<double>::pi * f0 / fs);
        const double Vh = std::pow (10.0, G / 20.0);
        const double Vb = std::pow (Vh, 0.4996667741545416);

        const double a0 = 1.0 + K / Q + K * K;
        const double b0 = (Vh + Vb * K / Q + K * K) / a0;
        const double b1 = 2.0 * (K * K - Vh) / a0;
        const double b2 = (Vh - Vb * K / Q + K * K) / a0;
        const double a1 = 2.0 * (K * K - 1.0) / a0;
        const double a2 = (1.0 - K / Q + K * K) / a0;

        return new juce::dsp::IIR::Coefficients<Sample> (
            static_cast<Sample> (b0), static_cast<Sample> (b1), static_cast<Sample> (b2),
            static_cast<Sample> (1.0), static_cast<Sample> (a1), static_cast<Sample> (a2));
    }

    static typename juce::dsp::IIR::Coefficients<Sample>::Ptr makeRlbCoeffs (double fs)
    {
        const double f0 = 38.13547087602444;
        const double Q  = 0.5003270373238773;

        const double K    = std::tan (juce::MathConstants<double>::pi * f0 / fs);
        const double a0raw = 1.0 + K / Q + K * K;

        const double b0 = 1.0 / a0raw;
        const double b1 = -2.0 / a0raw;
        const double b2 = 1.0 / a0raw;
        const double a1 = 2.0 * (K * K - 1.0) / a0raw;
        const double a2 = (1.0 - K / Q + K * K) / a0raw;

        return new juce::dsp::IIR::Coefficients<Sample> (
            static_cast<Sample> (b0), static_cast<Sample> (b1), static_cast<Sample> (b2),
            static_cast<Sample> (1.0), static_cast<Sample> (a1), static_cast<Sample> (a2));
    }

    // -70 LUFS in mean-square terms: 10^((-70 + 0.691) / 10)
    static constexpr double absoluteGateMeanSquare = 1.1724e-7;

    double sampleRate  = 44100.0;
    int    numChannels = 2;

    typename juce::dsp::IIR::Coefficients<Sample>::Ptr preCoeffs, rlbCoeffs;
    // Fixed at 2: this plugin only supports mono/stereo buses.
    std::array<juce::dsp::IIR::Filter<Sample>, 2> preFilter, rlbFilter;

    int    blockSamples        = 4410;
    int    samplesInBlock      = 0;
    int    channelsAccumulated = 2;
    double blockSumSquares     = 0.0;

    std::vector<double> momentaryBlocks, shortTermBlocks, gatingBlocks;

    std::atomic<float> momentary  { -100.0f };
    std::atomic<float> shortTerm  { -100.0f };
    std::atomic<float> integrated { -100.0f };
};
