#include "PluginEditor.h"

namespace
{
    const juce::Colour kBackground { 0xff1c1f24 };
    const juce::Colour kPanel      { 0xff262a31 };
    const juce::Colour kAccent     { 0xff4fc3f7 };
    const juce::Colour kAccentWarm { 0xffffb74d };
    const juce::Colour kText       { 0xffe8eaed };
    const juce::Colour kTrack      { 0xff3a3f47 };
}

//==============================================================================
PolishLookAndFeel::PolishLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, kText);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, kText);
    setColour (juce::ToggleButton::textColourId, kText);
    setColour (juce::ToggleButton::tickColourId, kAccent);
}

void PolishLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                          float sliderPos, float startAngle, float endAngle,
                                          juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<float> (x, y, width, height).reduced (4.0f);
    const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const auto angle  = startAngle + sliderPos * (endAngle - startAngle);
    const auto accent = slider.getProperties().getWithDefault ("warm", false)
                          ? kAccentWarm : kAccent;

    // body
    g.setColour (kPanel);
    g.fillEllipse (bounds);
    g.setColour (kTrack);
    g.drawEllipse (bounds, 1.5f);

    const auto arcRadius = radius - 4.0f;

    // background arc
    juce::Path back;
    back.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                        startAngle, endAngle, true);
    g.setColour (kTrack);
    g.strokePath (back, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));

    // value arc
    juce::Path value;
    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                         startAngle, angle, true);
    g.setColour (accent);
    g.strokePath (value, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    // pointer
    juce::Path pointer;
    const auto pointerLength = arcRadius * 0.6f;
    pointer.addRoundedRectangle (-1.5f, -pointerLength, 3.0f, pointerLength, 1.5f);
    pointer.applyTransform (juce::AffineTransform::rotation (angle).translated (centre));
    g.setColour (kText);
    g.fillPath (pointer);
}

//==============================================================================
LabeledKnob::LabeledKnob (juce::AudioProcessorValueTreeState& state,
                          const juce::String& paramID, const juce::String& caption)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 16);
    slider.setColour (juce::Slider::textBoxTextColourId, kText);
    addAndMakeVisible (slider);

    label.setText (caption, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    addAndMakeVisible (label);

    attachment = std::make_unique<Attachment> (state, paramID, slider);
}

void LabeledKnob::resized()
{
    auto area = getLocalBounds();
    label.setBounds (area.removeFromTop (18));
    slider.setBounds (area);
}

//==============================================================================
LevelMeter::LevelMeter (std::function<float()> source, bool fillFromTop, juce::String cap)
    : getValue (std::move (source)), fromTop (fillFromTop), caption (std::move (cap))
{
    startTimerHz (30);
}

LevelMeter::~LevelMeter() { stopTimer(); }

void LevelMeter::timerCallback()
{
    const auto raw = juce::jlimit (0.0f, 1.0f, getValue());
    // fast attack, slow release
    level = raw > level ? raw : level * 0.85f + raw * 0.15f;
    repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    auto area = getLocalBounds();
    auto labelArea = area.removeFromBottom (14);

    auto bounds = area.toFloat().reduced (1.0f);
    g.setColour (kTrack);
    g.fillRoundedRectangle (bounds, 3.0f);

    const auto filled = fromTop
        ? bounds.withBottom (bounds.getY() + bounds.getHeight() * level)
        : bounds.withTop (bounds.getBottom() - bounds.getHeight() * level);

    if (fromTop)
    {
        // gain reduction: cool -> hot as more is applied
        juce::ColourGradient grad (kAccent, bounds.getTopLeft(),
                                   juce::Colour (0xffe53935), bounds.getBottomLeft(), false);
        g.setGradientFill (grad);
    }
    else
    {
        juce::ColourGradient grad (juce::Colour (0xff66bb6a), bounds.getBottomLeft(),
                                   juce::Colour (0xffe53935), bounds.getTopLeft(), false);
        grad.addColour (0.75, juce::Colour (0xffffb74d));
        g.setGradientFill (grad);
    }
    g.fillRoundedRectangle (filled, 3.0f);

    g.setColour (kText);
    g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
    g.drawText (caption, labelArea, juce::Justification::centred);
}

//==============================================================================
AudioPolishEditor::AudioPolishEditor (AudioPolishProcessor& p)
    : AudioProcessorEditor (p), processor (p),
      inputKnob   (p.getValueTreeState(), ParamID::input,   "INPUT"),
      polishKnob  (p.getValueTreeState(), ParamID::polish,  "POLISH"),
      lowKnob     (p.getValueTreeState(), ParamID::low,     "LOW"),
      highKnob    (p.getValueTreeState(), ParamID::high,    "HIGH"),
      tiltKnob    (p.getValueTreeState(), ParamID::tilt,    "TILT"),
      driveKnob   (p.getValueTreeState(), ParamID::drive,   "DRIVE"),
      glueKnob    (p.getValueTreeState(), ParamID::glue,    "GLUE"),
      widthKnob   (p.getValueTreeState(), ParamID::width,   "WIDTH"),
      ceilingKnob (p.getValueTreeState(), ParamID::ceiling, "CEILING"),
      outputKnob  (p.getValueTreeState(), ParamID::output,  "OUTPUT"),
      mixKnob     (p.getValueTreeState(), ParamID::mix,     "MIX"),
      outMeter ([&p] { return juce::jlimit (0.0f, 1.0f,
                          juce::jmap (juce::Decibels::gainToDecibels (p.getOutputLevel(), -60.0f),
                                      -60.0f, 0.0f, 0.0f, 1.0f)); },
                false, "OUT"),
      grMeter  ([&p] { return juce::jlimit (0.0f, 1.0f,
                          juce::jmap (p.getGainReduction(), -24.0f, 0.0f, 1.0f, 0.0f)); },
                true, "GR")
{
    setLookAndFeel (&lookAndFeel);

    for (auto* k : { &inputKnob, &polishKnob, &lowKnob, &highKnob, &tiltKnob,
                     &driveKnob, &glueKnob, &widthKnob, &ceilingKnob, &outputKnob, &mixKnob })
        addAndMakeVisible (k);

    // The Polish macro gets the warm accent to mark it as the headline control.
    polishKnob.slider.getProperties().set ("warm", true);

    addAndMakeVisible (bypassButton);
    bypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>
                           (p.getValueTreeState(), ParamID::bypass, bypassButton);

    addAndMakeVisible (outMeter);
    addAndMakeVisible (grMeter);

    // Oversampling selector. The attachment fills the box from the parameter's
    // choices, so no items are added manually here.
    osLabel.setText ("OVERSAMPLING", juce::dontSendNotification);
    osLabel.setJustificationType (juce::Justification::centred);
    osLabel.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
    addAndMakeVisible (osLabel);

    osBox.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (osBox);
    osAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
                       (p.getValueTreeState(), ParamID::oversampling, osBox);

    setSize (640, 420);
    setResizable (true, true);
    setResizeLimits (520, 340, 1100, 720);
}

AudioPolishEditor::~AudioPolishEditor()
{
    setLookAndFeel (nullptr);
}

void AudioPolishEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);

    auto header = getLocalBounds().removeFromTop (54).toFloat();
    g.setColour (kPanel);
    g.fillRect (header);

    g.setColour (kText);
    g.setFont (juce::Font (juce::FontOptions (24.0f, juce::Font::bold)));
    g.drawText ("AUDIO  POLISH", header.reduced (18, 0).withTrimmedRight (60),
                juce::Justification::centredLeft);

    g.setColour (kAccent);
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    g.drawText ("one-knob finish", header.reduced (20, 0),
                juce::Justification::centredRight);
}

void AudioPolishEditor::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop (54);              // header
    area.reduce (12, 12);

    // right-hand column: GR + output meters, oversampling selector, then bypass
    auto right = area.removeFromRight (84);
    bypassButton.setBounds (right.removeFromBottom (28));
    right.removeFromBottom (6);
    osBox.setBounds (right.removeFromBottom (24));
    osLabel.setBounds (right.removeFromBottom (14));
    right.removeFromBottom (8);
    grMeter.setBounds  (right.removeFromLeft (right.getWidth() / 2).reduced (6, 0));
    outMeter.setBounds (right.reduced (6, 0));

    area.removeFromRight (12);

    // The Polish macro sits front-and-centre on its own row.
    auto top = area.removeFromTop (juce::roundToInt (area.getHeight() * 0.46f));
    polishKnob.setBounds (top.withSizeKeepingCentre (juce::jmin (180, top.getWidth()),
                                                     top.getHeight()));

    area.removeFromTop (8);

    // The remaining controls fill a responsive grid below.
    juce::Array<LabeledKnob*> grid {
        &inputKnob, &lowKnob, &highKnob, &tiltKnob, &driveKnob,
        &glueKnob, &widthKnob, &ceilingKnob, &outputKnob, &mixKnob
    };

    const int cols = 5;
    const int rows = (grid.size() + cols - 1) / cols;
    const int cellW = area.getWidth() / cols;
    const int cellH = area.getHeight() / rows;

    for (int i = 0; i < grid.size(); ++i)
    {
        const int r = i / cols;
        const int c = i % cols;
        grid[i]->setBounds (area.getX() + c * cellW,
                            area.getY() + r * cellH,
                            cellW, cellH);
    }
}
