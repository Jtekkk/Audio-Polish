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
    bypassParam  = apvts.getRawParameterValue (ParamID::bypass);
}

void AudioPolishProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlock);
    spec.numChannels      = static_cast<juce::uint32> (getTotalNumOutputChannels());

    // Prepare only the chain matching the host's requested precision; JUCE
    // re-calls prepareToPlay if the precision changes. Both share the same
    // oversampling latency.
    if (isUsingDoublePrecision())
    {
        doubleChain.prepare (spec);
        setLatencySamples (doubleChain.getLatencySamples());
    }
    else
    {
        floatChain.prepare (spec);
        setLatencySamples (floatChain.getLatencySamples());
    }
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
