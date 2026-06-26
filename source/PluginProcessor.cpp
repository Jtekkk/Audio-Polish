#include "PluginProcessor.h"
#include "PluginEditor.h"

AudioPolishProcessor::AudioPolishProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", AudioPolishParams::createLayout())
{
    inputParam   = apvts.getRawParameterValue (ParamID::input);
    polishParam  = apvts.getRawParameterValue (ParamID::polish);
    lowParam     = apvts.getRawParameterValue (ParamID::low);
    highParam    = apvts.getRawParameterValue (ParamID::high);
    tiltParam    = apvts.getRawParameterValue (ParamID::tilt);
    driveParam   = apvts.getRawParameterValue (ParamID::drive);
    glueParam    = apvts.getRawParameterValue (ParamID::glue);
    widthParam   = apvts.getRawParameterValue (ParamID::width);
    ceilingParam = apvts.getRawParameterValue (ParamID::ceiling);
    outputParam  = apvts.getRawParameterValue (ParamID::output);
    mixParam     = apvts.getRawParameterValue (ParamID::mix);
    osParam      = apvts.getRawParameterValue (ParamID::oversampling);
    bypassParam  = apvts.getRawParameterValue (ParamID::bypass);

    apvts.addParameterListener (ParamID::oversampling, this);
}

AudioPolishProcessor::~AudioPolishProcessor()
{
    apvts.removeParameterListener (ParamID::oversampling, this);
    cancelPendingUpdate();
}

int AudioPolishProcessor::oversamplingExponent() const noexcept
{
    // Choice indices map directly to the oversampling exponent: 0=Off, 1=2x, 2=4x, 3=8x.
    return juce::jlimit (0, 3, juce::roundToInt (osParam->load()));
}

void AudioPolishProcessor::prepareChains (bool reportLatencyToHost)
{
    if (lastSampleRate <= 0.0)
        return;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = lastSampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (lastBlockSize);
    spec.numChannels      = static_cast<juce::uint32> (getTotalNumOutputChannels());

    const auto os = oversamplingExponent();
    preparedOs = os;

    // Prepare only the chain matching the host's requested precision; JUCE
    // re-calls prepareToPlay if the precision changes. Both share the same
    // oversampling latency. The chain's internal dry/bypass delays always track
    // its own latency, so they stay aligned; we only push the latency to the
    // host at prepareToPlay time (changing reported latency mid-stream makes
    // some hosts re-initialise re-entrantly, which is unsafe during automation).
    int latency = 0;
    if (isUsingDoublePrecision())
    {
        doubleChain.prepare (spec, os);
        latency = doubleChain.getLatencySamples();
    }
    else
    {
        floatChain.prepare (spec, os);
        latency = floatChain.getLatencySamples();
    }

    if (reportLatencyToHost)
        setLatencySamples (latency);
}

void AudioPolishProcessor::parameterChanged (const juce::String& paramID, float)
{
    // Called from any thread; re-prepare on the message thread where allocation
    // (and the wrapper's suspend handling) is safe.
    if (paramID == ParamID::oversampling)
        triggerAsyncUpdate();
}

void AudioPolishProcessor::handleAsyncUpdate()
{
    if (lastSampleRate <= 0.0 || oversamplingExponent() == preparedOs)
        return;   // nothing to do if the factor hasn't actually changed

    // Re-preparing reallocates the oversampler. Hold the callback lock so it
    // can't race a concurrent processBlock — some hosts and validators call
    // processBlock directly without honouring suspendProcessing. Latency is NOT
    // re-reported here (see prepareChains): the host keeps the value from the
    // last prepareToPlay, avoiding a re-entrant re-init during automation.
    const juce::ScopedLock sl (getCallbackLock());
    prepareChains (/*reportLatencyToHost*/ false);
}

void AudioPolishProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    lastSampleRate = sampleRate;
    lastBlockSize  = samplesPerBlock;
    prepareChains();
}

bool AudioPolishProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& main = layouts.getMainOutputChannelSet();

    if (main != juce::AudioChannelSet::mono()
        && main != juce::AudioChannelSet::stereo())
        return false;

    // Input and output layouts must match.
    return main == layouts.getMainInputChannelSet();
}

PolishSettings AudioPolishProcessor::readSettings() const
{
    PolishSettings s;
    s.inputDb   = inputParam->load();
    s.polish    = polishParam->load();
    s.lowDb     = lowParam->load();
    s.highDb    = highParam->load();
    s.tiltDb    = tiltParam->load();
    s.drive     = driveParam->load();
    s.glue      = glueParam->load();
    s.width     = widthParam->load();
    s.ceilingDb = ceilingParam->load();
    s.outputDb  = outputParam->load();
    s.mix       = mixParam->load();
    return s;
}

template <typename Sample>
void AudioPolishProcessor::processChain (juce::AudioBuffer<Sample>& buffer,
                                         PolishChain<Sample>& chain)
{
    juce::ScopedNoDenormals noDenormals;

    // Serialise against handleAsyncUpdate() re-preparing the chain when the
    // oversampling setting changes. Uncontended this is a cheap lock; it only
    // blocks briefly while the chain is being rebuilt.
    const juce::ScopedLock sl (getCallbackLock());

    // Clear any output channels that don't carry input.
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (bypassParam->load() > 0.5f)
    {
        chain.processBypassed (buffer);
        return;
    }

    chain.setSettings (readSettings());
    chain.process (buffer);
}

void AudioPolishProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    processChain (buffer, floatChain);
}

void AudioPolishProcessor::processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer&)
{
    processChain (buffer, doubleChain);
}

//==============================================================================
// Factory presets. Each lists the sonic parameters it sets (in real units);
// anything unlisted keeps its current value. Oversampling and bypass are
// deliberately left untouched.
namespace
{
    struct Preset
    {
        juce::String name;
        std::vector<std::pair<juce::String, float>> values;
    };

    const std::vector<Preset>& getPresets()
    {
        using namespace ParamID;
        static const std::vector<Preset> presets =
        {
            { "Init / Flat",
              { { input, 0 }, { polish, 0 }, { low, 0 }, { high, 0 }, { tilt, 0 },
                { drive, 0 }, { glue, 0 }, { width, 100 }, { ceiling, -0.3f },
                { output, 0 }, { mix, 100 } } },

            { "Subtle Polish",
              { { polish, 25 }, { drive, 15 }, { glue, 20 }, { high, 1.0f },
                { width, 105 }, { ceiling, -0.3f }, { mix, 100 } } },

            { "Glue Bus",
              { { polish, 30 }, { glue, 55 }, { drive, 15 }, { low, 1.0f },
                { ceiling, -0.5f }, { width, 100 }, { mix, 100 } } },

            { "Warm Master",
              { { polish, 40 }, { drive, 35 }, { glue, 30 }, { low, 1.5f },
                { high, 1.0f }, { tilt, -1.0f }, { width, 110 }, { ceiling, -0.3f } } },

            { "Wide & Bright",
              { { polish, 35 }, { drive, 20 }, { glue, 20 }, { high, 3.0f },
                { tilt, 2.0f }, { width, 140 }, { ceiling, -0.3f } } },

            { "Loud & Proud",
              { { polish, 60 }, { drive, 40 }, { glue, 45 }, { output, 2.0f },
                { width, 110 }, { ceiling, -0.2f }, { mix, 100 } } },
        };
        return presets;
    }
}

int AudioPolishProcessor::getNumPrograms()            { return static_cast<int> (getPresets().size()); }
int AudioPolishProcessor::getCurrentProgram()         { return currentProgram; }

const juce::String AudioPolishProcessor::getProgramName (int index)
{
    if (juce::isPositiveAndBelow (index, getNumPrograms()))
        return getPresets()[static_cast<size_t> (index)].name;
    return {};
}

void AudioPolishProcessor::setCurrentProgram (int index)
{
    if (! juce::isPositiveAndBelow (index, getNumPrograms()))
        return;

    currentProgram = index;

    for (const auto& [id, value] : getPresets()[static_cast<size_t> (index)].values)
        if (auto* p = apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (value));

    updateHostDisplay();
    sendChangeMessage();   // let the editor refresh its preset selector
}

juce::AudioProcessorEditor* AudioPolishProcessor::createEditor()
{
    return new AudioPolishEditor (*this);
}

void AudioPolishProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto state = apvts.copyState(); state.isValid())
    {
        const auto xml = state.createXml();
        copyXmlToBinary (*xml, destData);
    }
}

void AudioPolishProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AudioPolishProcessor();
}
