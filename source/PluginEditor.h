#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

/** Dark, slightly glossy rotary look used throughout the plugin. */
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

    juce::Slider slider;

private:
    juce::Label label;
    std::unique_ptr<Attachment> attachment;
};

/** Thin vertical output meter. */
class LevelMeter : public juce::Component, private juce::Timer
{
public:
    explicit LevelMeter (AudioPolishProcessor& p);
    ~LevelMeter() override;

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    AudioPolishProcessor& processor;
    float level = 0.0f; // smoothed, in dB-ish 0..1 display range
};

class AudioPolishEditor : public juce::AudioProcessorEditor
{
public:
    explicit AudioPolishEditor (AudioPolishProcessor&);
    ~AudioPolishEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    AudioPolishProcessor& processor;
    PolishLookAndFeel lookAndFeel;

    LabeledKnob inputKnob, polishKnob, lowKnob, highKnob, tiltKnob,
                driveKnob, glueKnob, widthKnob, ceilingKnob, outputKnob;

    juce::ToggleButton bypassButton { "Bypass" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment;

    LevelMeter meter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPolishEditor)
};
