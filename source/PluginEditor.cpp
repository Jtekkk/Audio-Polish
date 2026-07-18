#include "PluginEditor.h"

namespace
{
    const juce::Colour kBackground { 0xff1c1f24 };
    const juce::Colour kPanel      { 0xff262a31 };
    const juce::Colour kAccent     { 0xff4fc3f7 };
    const juce::Colour kAccentWarm { 0xffffb74d };
    const juce::Colour kText       { 0xffe8eaed };
    const juce::Colour kTrack      { 0xff3a3f47 };

    // Section accent hues (also used for knob arcs / pointers within each group).
    const juce::Colour kToneHue  { 0xff4fc3f7 }; // INPUT & TONE
    const juce::Colour kDriveHue { 0xffffb74d }; // DRIVE & GLUE
    const juce::Colour kWidthHue { 0xff4dd0a8 }; // WIDTH
    const juce::Colour kOutHue   { 0xffba68c8 }; // OUTPUT
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

    const auto defaultArgb = juce::var (static_cast<juce::int64> (kAccent.getARGB()));
    const auto storedArgb  = static_cast<juce::int64> (slider.getProperties().getWithDefault ("accentColour", defaultArgb));
    const auto accent = juce::Colour (static_cast<juce::uint32> (storedArgb));

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

void LabeledKnob::setAccentColour (juce::Colour colour)
{
    slider.getProperties().set ("accentColour", juce::var (static_cast<juce::int64> (colour.getARGB())));
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
CorrelationMeter::CorrelationMeter (std::function<float()> source)
    : getValue (std::move (source))
{
    startTimerHz (15);
}

CorrelationMeter::~CorrelationMeter() { stopTimer(); }

void CorrelationMeter::timerCallback()
{
    value = juce::jlimit (-1.0f, 1.0f, getValue());
    repaint();
}

void CorrelationMeter::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (kTrack);
    g.fillRoundedRectangle (bounds, 3.0f);

    const auto centreX     = bounds.getCentreX();
    const auto half        = bounds.getWidth() * 0.5f;
    const auto filledWidth = half * std::abs (value);

    const auto filled = value >= 0.0f
        ? juce::Rectangle<float> (centreX, bounds.getY(), filledWidth, bounds.getHeight())
        : juce::Rectangle<float> (centreX - filledWidth, bounds.getY(), filledWidth, bounds.getHeight());

    g.setColour (value >= 0.0f ? juce::Colour (0xff66bb6a) : juce::Colour (0xffe53935));
    g.fillRoundedRectangle (filled, 3.0f);

    g.setColour (kText.withAlpha (0.5f));
    g.drawLine (centreX, bounds.getY(), centreX, bounds.getBottom(), 1.0f);
}

//==============================================================================
NumericReadout::NumericReadout (std::function<float()> source, juce::String cap, juce::String suffix)
    : getValue (std::move (source)), caption (std::move (cap)), unit (std::move (suffix))
{
    startTimerHz (8);
}

NumericReadout::~NumericReadout() { stopTimer(); }

void NumericReadout::timerCallback()
{
    displayValue = getValue();
    repaint();
}

void NumericReadout::mouseUp (const juce::MouseEvent&)
{
    if (onClick != nullptr)
        onClick();
}

void NumericReadout::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    g.setColour (kTrack.withAlpha (0.6f));
    g.fillRoundedRectangle (area, 4.0f);

    auto captionArea = area.removeFromTop (area.getHeight() * 0.42f);
    g.setColour (kText.withAlpha (0.65f));
    g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
    g.drawText (caption, captionArea, juce::Justification::centred);

    g.setColour (kText);
    g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
    const auto text = displayValue <= -99.0f ? juce::String ("--")
                                              : juce::String (displayValue, 1) + " " + unit;
    g.drawText (text, area, juce::Justification::centred);
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
                true, "GR"),
      correlationMeter ([&p] { return p.getCorrelation(); }),
      lufsShortReadout      ([&p] { return p.getLufsShortTerm();  }, "LUFS-S",    "LU"),
      lufsIntegratedReadout ([&p] { return p.getLufsIntegrated(); }, "LUFS-I",    "LU"),
      truePeakReadout       ([&p] { return p.getTruePeakDb();     }, "TRUE PEAK", "dBTP")
{
    setLookAndFeel (&lookAndFeel);

    for (auto* k : { &inputKnob, &polishKnob, &lowKnob, &highKnob, &tiltKnob,
                     &driveKnob, &glueKnob, &widthKnob, &ceilingKnob, &outputKnob, &mixKnob })
        addAndMakeVisible (k);

    // Colour-code each knob to match the section it visually sits in; the
    // Polish macro gets its own warm accent to mark it as the headline control.
    polishKnob.setAccentColour (kAccentWarm);
    for (auto* k : { &inputKnob, &lowKnob, &highKnob, &tiltKnob })
        k->setAccentColour (kToneHue);
    for (auto* k : { &driveKnob, &glueKnob })
        k->setAccentColour (kDriveHue);
    widthKnob.setAccentColour (kWidthHue);
    for (auto* k : { &ceilingKnob, &outputKnob, &mixKnob })
        k->setAccentColour (kOutHue);

    addAndMakeVisible (bypassButton);
    bypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>
                           (p.getValueTreeState(), ParamID::bypass, bypassButton);

    addAndMakeVisible (abMatchButton);
    abMatchAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>
                            (p.getValueTreeState(), ParamID::abMatch, abMatchButton);

    addAndMakeVisible (ditherButton);
    ditherAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>
                           (p.getValueTreeState(), ParamID::ditherOn, ditherButton);

    // Dither bit-depth selector (attachment fills it from the parameter's choices).
    addAndMakeVisible (ditherBitsBox);
    ditherBitsAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
                                (p.getValueTreeState(), ParamID::ditherBits, ditherBitsBox);

    addAndMakeVisible (outMeter);
    addAndMakeVisible (grMeter);
    addAndMakeVisible (correlationMeter);
    addAndMakeVisible (lufsShortReadout);
    addAndMakeVisible (lufsIntegratedReadout);
    addAndMakeVisible (truePeakReadout);

    // Click LUFS-I to reset the integrated-loudness history ("start of song").
    lufsIntegratedReadout.onClick = [&p] { p.resetLoudnessIntegration(); };

    // Oversampling selector (attachment fills the box from the parameter's
    // choices, so no items are added manually here).
    osLabel.setText ("OVERSAMPLING", juce::dontSendNotification);
    osLabel.setJustificationType (juce::Justification::centred);
    osLabel.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
    addAndMakeVisible (osLabel);

    osBox.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (osBox);
    osAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
                       (p.getValueTreeState(), ParamID::oversampling, osBox);

    // Factory + user preset selector (driven by the processor's program
    // interface plus any presets saved to disk via the Save button).
    presetBox.setJustificationType (juce::Justification::centredLeft);
    presetBox.setTextWhenNothingSelected ("Presets");
    addAndMakeVisible (presetBox);
    refreshPresetBox();
    presetBox.onChange = [this]
    {
        const auto id = presetBox.getSelectedId();
        if (id <= 0)
            return;

        if (id >= 1000)
            processor.loadUserPreset (presetBox.getText());
        else
            processor.setCurrentProgram (id - 1);
    };
    processor.addChangeListener (this);

    addAndMakeVisible (saveButton);
    saveButton.onClick = [this] { showSavePresetPrompt(); };

    setSize (940, 640);
    setResizable (true, true);
    setResizeLimits (720, 480, 1500, 980);
}

AudioPolishEditor::~AudioPolishEditor()
{
    processor.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void AudioPolishEditor::refreshPresetBox()
{
    presetBox.clear (juce::dontSendNotification);
    for (int i = 0; i < processor.getNumPrograms(); ++i)
        presetBox.addItem (processor.getProgramName (i), i + 1);   // ids are 1-based

    const auto userNames = processor.getUserPresetNames();
    if (! userNames.isEmpty())
    {
        presetBox.addSeparator();
        int id = 1000;
        for (auto& name : userNames)
            presetBox.addItem (name, id++);
    }

    presetBox.setSelectedId (processor.getCurrentProgram() + 1, juce::dontSendNotification);
}

void AudioPolishEditor::showSavePresetPrompt()
{
    savePrompt = std::make_unique<juce::AlertWindow> ("Save Preset", "Name this preset:",
                                                       juce::MessageBoxIconType::NoIcon);
    savePrompt->addTextEditor ("name", "My Preset");
    savePrompt->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    savePrompt->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<AudioPolishEditor> safeThis (this);
    savePrompt->enterModalState (true, juce::ModalCallbackFunction::create (
        [safeThis] (int result)
        {
            if (safeThis == nullptr)
                return;

            if (result == 1)
            {
                const auto name = safeThis->savePrompt->getTextEditorContents ("name").trim();
                if (name.isNotEmpty() && safeThis->processor.saveUserPreset (name))
                    safeThis->refreshPresetBox();
            }

            safeThis->savePrompt.reset();
        }));
}

void AudioPolishEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // The processor switched program (e.g. from the host); reflect it.
    presetBox.setSelectedId (processor.getCurrentProgram() + 1, juce::dontSendNotification);
}

void AudioPolishEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);

    auto header = getLocalBounds().removeFromTop (60).toFloat();
    g.setColour (kPanel);
    g.fillRect (header);

    g.setColour (kText);
    g.setFont (juce::Font (juce::FontOptions (26.0f, juce::Font::bold)));
    g.drawText ("AUDIO  POLISH", header.reduced (20, 0).withTrimmedRight (290),
                juce::Justification::centredLeft);

    for (auto& s : sections)
    {
        g.setColour (s.colour.withAlpha (0.08f));
        g.fillRoundedRectangle (s.bounds, 8.0f);
        g.setColour (s.colour.withAlpha (0.35f));
        g.drawRoundedRectangle (s.bounds, 8.0f, 1.2f);

        g.setColour (s.colour.brighter (0.3f));
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText (s.title, s.bounds.reduced (10.0f, 4.0f).removeFromTop (16.0f),
                    juce::Justification::centredLeft);
    }
}

void AudioPolishEditor::resized()
{
    auto area = getLocalBounds();
    auto header = area.removeFromTop (60);
    presetBox.setBounds (header.removeFromRight (200).reduced (12, 14));
    saveButton.setBounds (header.removeFromRight (72).reduced (6, 14));
    area.reduce (14, 14);

    // Right-hand rail: meters, LUFS/True-Peak readouts, then the toggles.
    auto right = area.removeFromRight (152);

    bypassButton.setBounds (right.removeFromBottom (26));
    right.removeFromBottom (6);
    abMatchButton.setBounds (right.removeFromBottom (26));
    right.removeFromBottom (10);

    ditherBitsBox.setBounds (right.removeFromBottom (24));
    ditherButton.setBounds (right.removeFromBottom (24));
    right.removeFromBottom (10);

    osBox.setBounds (right.removeFromBottom (24));
    osLabel.setBounds (right.removeFromBottom (14));
    right.removeFromBottom (10);

    truePeakReadout.setBounds (right.removeFromBottom (34));
    right.removeFromBottom (4);
    lufsIntegratedReadout.setBounds (right.removeFromBottom (34));
    right.removeFromBottom (4);
    lufsShortReadout.setBounds (right.removeFromBottom (34));
    right.removeFromBottom (8);

    correlationMeter.setBounds (right.removeFromBottom (18));
    right.removeFromBottom (8);

    grMeter.setBounds  (right.removeFromLeft (right.getWidth() / 2).reduced (6, 0));
    outMeter.setBounds (right.reduced (6, 0));

    area.removeFromRight (14);

    // The Polish macro sits front-and-centre on its own row.
    auto top = area.removeFromTop (juce::roundToInt (area.getHeight() * 0.4f));
    polishKnob.setBounds (top.withSizeKeepingCentre (juce::jmin (200, top.getWidth()),
                                                     top.getHeight()));

    area.removeFromTop (10);

    // Four colour-coded sections, sized proportionally to how many knobs
    // they hold, filling the remaining space.
    struct Group { juce::Array<LabeledKnob*> knobs; juce::Colour colour; juce::String title; };
    const std::vector<Group> groups {
        { { &inputKnob, &lowKnob, &highKnob, &tiltKnob }, kToneHue,  "INPUT & TONE" },
        { { &driveKnob, &glueKnob },                      kDriveHue, "DRIVE & GLUE" },
        { { &widthKnob },                                 kWidthHue, "WIDTH" },
        { { &ceilingKnob, &outputKnob, &mixKnob },        kOutHue,   "OUTPUT" },
    };

    constexpr int totalKnobs = 10;
    constexpr int gap = 8;
    const int totalWidth = juce::jmax (0, area.getWidth() - gap * static_cast<int> (groups.size() - 1));

    sections.clear();
    int x = area.getX();
    for (auto& grp : groups)
    {
        const int w = juce::roundToInt (totalWidth * (float) grp.knobs.size() / (float) totalKnobs);
        juce::Rectangle<int> grpArea (x, area.getY(), w, area.getHeight());

        sections.push_back ({ grpArea.toFloat(), grp.colour, grp.title });

        auto inner = grpArea.reduced (8, 8);
        inner.removeFromTop (16); // room for the section title painted in paint()

        const int cellW = grp.knobs.isEmpty() ? inner.getWidth() : inner.getWidth() / grp.knobs.size();
        for (int i = 0; i < grp.knobs.size(); ++i)
            grp.knobs[i]->setBounds (inner.getX() + i * cellW, inner.getY(), cellW, inner.getHeight());

        x += w + gap;
    }
}
