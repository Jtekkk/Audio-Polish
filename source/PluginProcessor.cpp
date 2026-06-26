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

void AudioPolishProcessor::prepareChains()
{
    if (lastSampleRate <= 0.0)
        return;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = lastSampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (lastBlockSize);
    spec.numChannels      = static_cast<juce::uint32> (getTotalNumOutputChannels());

    const auto os = oversamplingExponent();

    // Prepare only the chain matching the host's requested precision; JUCE
    // re-calls prepareToPlay if the precision changes. Both share the same
    // oversampling latency.
    if (isUsingDoublePrecision())
    {
        doubleChain.prepare (spec, os);
        setLatencySamples (doubleChain.getLatencySamples());
    }
    else
    {
        floatChain.prepare (spec, os);
        setLatencySamples (floatChain.getLatencySamples());
    }
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
    if (lastSampleRate <= 0.0)
        return;

    suspendProcessing (true);
    prepareChains();
    suspendProcessing (false);
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
