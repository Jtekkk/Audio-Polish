#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <vector>
#include "PluginProcessor.h"

/** Dark, slightly glossy rotary look used throughout the plugin. Each knob can
    carry its own accent colour via a Component property so knob groups can be
    colour-coded. */
class PolishLookAndFeel : public juce::LookAndFeel_V4
{
public:
    PolishLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider&) override;
};

/** A rotary slider plus a caption, wired to an APVTS parameter. */
class LabeledKnob : public juce::Component
{
public:
    using Attachment = juce::AudioProcessorValueTreeState::SliderAttachment;

    LabeledKnob (juce::AudioProcessorValueTreeState& state,
                 const juce::String& paramID, const juce::String& caption);

    void resized() override;

    /** Tints this knob's arc/pointer with the given colour (section colour-coding). */
    void setAccentColour (juce::Colour colour);

    juce::Slider slider;

private:
    juce::Label label;
    std::unique_ptr<Attachment> attachment;
};

/** Thin vertical meter driven by a caller-supplied 0..1 source.

    Used both as an output level meter (fills from the bottom) and as a gain
    reduction meter (fills from the top). */
class LevelMeter : public juce::Component, private juce::Timer
{
public:
    LevelMeter (std::function<float()> source, bool fillFromTop, juce::String caption);
    ~LevelMeter() override;

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    std::function<float()> getValue;
    bool fromTop;
    juce::String caption;
    float level = 0.0f; // smoothed 0..1 display range
};

/** Thin horizontal bipolar meter for stereo phase correlation: -1 (out of
    phase, red) .. 0 (centre) .. +1 (mono-safe, green). */
class CorrelationMeter : public juce::Component, private juce::Timer
{
public:
    explicit CorrelationMeter (std::function<float()> source);
    ~CorrelationMeter() override;

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    std::function<float()> getValue;
    float value = 1.0f;
};

/** Small caption-over-value numeric readout, refreshed on a timer. Used for
    the LUFS and True Peak displays. */
class NumericReadout : public juce::Component, private juce::Timer
{
public:
    NumericReadout (std::function<float()> source, juce::String caption, juce::String suffix);
    ~NumericReadout() override;

    void paint (juce::Graphics&) override;

    /** Fires on click (e.g. to reset LUFS-Integrated history). */
    std::function<void()> onClick;
    void mouseUp (const juce::MouseEvent&) override;

private:
    void timerCallback() override;

    std::function<float()> getValue;
    juce::String caption, unit;
    float displayValue = -100.0f;
};

class AudioPolishEditor : public juce::AudioProcessorEditor,
                          private juce::ChangeListener
{
public:
    explicit AudioPolishEditor (AudioPolishProcessor&);
    ~AudioPolishEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refreshPresetBox();
    void showSavePresetPrompt();

    AudioPolishProcessor& processor;
    PolishLookAndFeel lookAndFeel;

    juce::ComboBox presetBox;
    juce::TextButton saveButton { "Save" };
    std::unique_ptr<juce::AlertWindow> savePrompt;

    LabeledKnob inputKnob, polishKnob, lowKnob, highKnob, tiltKnob,
                driveKnob, glueKnob, widthKnob, ceilingKnob, outputKnob, mixKnob;

    // Section backdrops, colour-coded and computed in resized(); painted first
    // so the knob components sit visually "inside" them.
    struct Section { juce::Rectangle<float> bounds; juce::Colour colour; juce::String title; };
    std::vector<Section> sections;

    juce::ToggleButton bypassButton  { "Bypass" };
    juce::ToggleButton abMatchButton { "Loudness Match" };
    juce::ToggleButton ditherButton  { "Dither" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> abMatchAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> ditherAttachment;

    juce::Label osLabel;
    juce::ComboBox osBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> osAttachment;

    juce::ComboBox ditherBitsBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> ditherBitsAttachment;

    LevelMeter outMeter, grMeter;
    CorrelationMeter correlationMeter;
    NumericReadout lufsShortReadout, lufsIntegratedReadout, truePeakReadout;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPolishEditor)
};
