#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "ParameterIDs.h"
#include "PolishChain.h"

class AudioPolishProcessor : public juce::AudioProcessor,
                             public  juce::ChangeBroadcaster,
                             private juce::AudioProcessorValueTreeState::Listener,
                             private juce::AsyncUpdater
{
public:
    AudioPolishProcessor();
    ~AudioPolishProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    bool supportsDoublePrecisionProcessing() const override { return true; }
    void processBlock (juce::AudioBuffer<float>&,  juce::MidiBuffer&) override;
    void processBlock (juce::AudioBuffer<double>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Audio Polish"; }

    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getValueTreeState() noexcept { return apvts; }
    float getOutputLevel() const noexcept
    {
        return isUsingDoublePrecision() ? doubleChain.getOutputLevel() : floatChain.getOutputLevel();
    }
    float getGainReduction() const noexcept
    {
        return isUsingDoublePrecision() ? doubleChain.getGainReduction() : floatChain.getGainReduction();
    }

private:
    PolishSettings readSettings() const;
    int  oversamplingExponent() const noexcept;
    void prepareChains();

    // Re-prepares the chain off the audio thread when the oversampling setting
    // changes (re-preparing allocates and alters latency).
    void parameterChanged (const juce::String& paramID, float newValue) override;
    void handleAsyncUpdate() override;

    template <typename Sample>
    void processChain (juce::AudioBuffer<Sample>&, PolishChain<Sample>&);

    juce::AudioProcessorValueTreeState apvts;
    PolishChain<float>  floatChain;
    PolishChain<double> doubleChain;

    double lastSampleRate = 0.0;
    int    lastBlockSize  = 0;
    int    currentProgram = 0;

    std::atomic<float>* inputParam   = nullptr;
    std::atomic<float>* polishParam  = nullptr;
    std::atomic<float>* lowParam     = nullptr;
    std::atomic<float>* highParam    = nullptr;
    std::atomic<float>* tiltParam    = nullptr;
    std::atomic<float>* driveParam   = nullptr;
    std::atomic<float>* glueParam    = nullptr;
    std::atomic<float>* widthParam   = nullptr;
    std::atomic<float>* ceilingParam = nullptr;
    std::atomic<float>* outputParam  = nullptr;
    std::atomic<float>* mixParam     = nullptr;
    std::atomic<float>* osParam      = nullptr;
    std::atomic<float>* bypassParam  = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPolishProcessor)
};
