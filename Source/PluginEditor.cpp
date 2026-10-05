#include "PluginEditor.h"

namespace
{
float smoothstep (float a, float b, float x) noexcept
{
    const float t = juce::jlimit (0.0f, 1.0f, (x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}
} // namespace

//==============================================================================
AlphaLookAndFeel::AlphaLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, AlphaColours::charcoal);

    setColour (juce::Slider::textBoxTextColourId, AlphaColours::text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, AlphaColours::text);

    setColour (juce::ComboBox::backgroundColourId, AlphaColours::panel);
    setColour (juce::ComboBox::textColourId, AlphaColours::text);
    setColour (juce::ComboBox::outlineColourId, AlphaColours::panelEdge);
    setColour (juce::ComboBox::arrowColourId, AlphaColours::amber);

    setColour (juce::PopupMenu::backgroundColourId, AlphaColours::panel);
    setColour (juce::PopupMenu::textColourId, AlphaColours::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, AlphaColours::amberDim);
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
}

void AlphaLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                         float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                                         juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (4.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    // Track + value arc.
    const float arcR = radius - 2.0f;
    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (AlphaColours::panelEdge);
    g.strokePath (track, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    juce::Path value;
    value.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, rotaryStartAngle, angle, true);
    g.setColour (slider.isEnabled() ? AlphaColours::amber : AlphaColours::amberDim);
    g.strokePath (value, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Knob body.
    const float bodyR = radius - 8.0f;
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3d3d47), centre.x, centre.y - bodyR,
                                             juce::Colour (0xff1b1b20), centre.x, centre.y + bodyR, false));
    g.fillEllipse (centre.x - bodyR, centre.y - bodyR, bodyR * 2.0f, bodyR * 2.0f);
    g.setColour (AlphaColours::panelEdge);
    g.drawEllipse (centre.x - bodyR, centre.y - bodyR, bodyR * 2.0f, bodyR * 2.0f, 1.0f);

    // Pointer.
    juce::Path pointer;
    pointer.addRoundedRectangle (-1.5f, -bodyR + 3.0f, 3.0f, bodyR * 0.45f, 1.5f);
    g.setColour (AlphaColours::amber);
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre.x, centre.y));
}

//==============================================================================
LampComponent::LampComponent (AlphaLavaAudioProcessor& p)
    : processor (p),
      fieldImage (juce::Image::ARGB, fieldW, fieldH, true)
{
    setOpaque (false);
}

// Draws the wax as metaballs into a small image. Scaled up with high-quality resampling it
// reads as smooth, soft-edged blobs. Blob positions come from the audio thread's snapshot,
// so what you see is exactly what is steering the filters.
void LampComponent::renderField (float waxHue, float level, float aspect, float topToBottomWidth)
{
    const auto& snap = processor.getSnapshot();
    const int count = juce::jlimit (0, lava::maxBlobs, snap.count.load (std::memory_order_relaxed));

    struct Ball { float cx, cy, r, stretch; };
    std::array<Ball, (size_t) lava::maxBlobs + 1> balls {};
    int n = 0;

    // The glass tapers, so squeeze blob x/size toward the middle as they rise.
    const auto widthAt = [topToBottomWidth] (float yFrac) { return 1.0f - (1.0f - topToBottomWidth) * yFrac; };

    for (int i = 0; i < count; ++i)
    {
        const float by = snap.y[(size_t) i].load (std::memory_order_relaxed);
        const float bx = snap.x[(size_t) i].load (std::memory_order_relaxed);
        const float vy = snap.vy[(size_t) i].load (std::memory_order_relaxed);
        const float r = snap.radius[(size_t) i].load (std::memory_order_relaxed);
        const float w = widthAt (by);

        // Fast-moving blobs stretch vertically.
        balls[(size_t) n++] = { 0.5f + (bx - 0.5f) * w, by * aspect, r * w,
                                1.0f / (1.0f + 0.25f * juce::jmin (2.0f, std::abs (vy))) };
    }

    // The pool of wax sitting on the bulb. Blobs merge into it and pull away from it.
    balls[(size_t) n++] = { 0.5f, -0.10f * aspect, 0.40f, 1.0f };

    const float liquidHue = std::fmod (waxHue + 0.10f, 1.0f);
    juce::Image::BitmapData bitmap (fieldImage, juce::Image::BitmapData::writeOnly);

    for (int py = 0; py < fieldH; ++py)
    {
        const float yFrac = 1.0f - ((float) py + 0.5f) / (float) fieldH;
        const float Y = yFrac * aspect;
        const float inv = 1.0f - yFrac;
        const float glow = inv * inv * 0.6f + 0.2f + 0.45f * level;   // the bulb is at the bottom

        const auto liquid = juce::Colour::fromHSV (liquidHue, 0.70f,
                                                   juce::jlimit (0.0f, 1.0f, 0.12f + 0.55f * glow), 1.0f);

        for (int px = 0; px < fieldW; ++px)
        {
            const float X = ((float) px + 0.5f) / (float) fieldW;
            float f = 0.0f;

            for (int k = 0; k < n; ++k)
            {
                const auto& b = balls[(size_t) k];
                const float dx = X - b.cx;
                const float dy = (Y - b.cy) * b.stretch;
                f += (b.r * b.r) / (dx * dx + dy * dy + 1.0e-4f);
            }

            const float alpha = smoothstep (0.9f, 1.25f, f);
            const float waxV = juce::jlimit (0.0f, 1.0f, 0.40f + 0.30f * juce::jmin (1.0f, (f - 1.0f) * 0.5f) + 0.45f * glow);
            const auto wax = juce::Colour::fromHSV (waxHue, 0.85f, waxV, 1.0f);

            bitmap.setPixelColour (px, py, liquid.interpolatedWith (wax, alpha));
        }
    }
}

void LampComponent::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat().reduced (6.0f);
    const float W = area.getWidth();
    const float H = area.getHeight();
    const float cx = area.getCentreX();

    const float capH = H * 0.08f;
    const float baseH = H * 0.20f;
    const float glassTop = area.getY() + capH;
    const float glassBot = area.getBottom() - baseH;
    const float glassH = glassBot - glassTop;
    const float topW = W * 0.44f;
    const float botW = W * 0.82f;

    const auto& snap = processor.getSnapshot();
    const float level = snap.level.load (std::memory_order_relaxed);
    const float hue = processor.apvts.getRawParameterValue (ParamIDs::hue)->load();

    // Room glow: the lamp lights up the desk, more so when it is working hard.
    {
        const auto glowCol = juce::Colour::fromHSV (std::fmod (hue + 0.08f, 1.0f), 0.8f, 1.0f, 0.10f + 0.28f * level);
        const float gy = glassBot - glassH * 0.25f;
        g.setGradientFill (juce::ColourGradient (glowCol, cx, gy, glowCol.withAlpha (0.0f), cx + W * 0.95f, gy, true));
        g.fillRect (getLocalBounds());
    }

    // Cap.
    {
        juce::Path cap;
        cap.startNewSubPath (cx - topW * 0.36f, area.getY());
        cap.lineTo (cx + topW * 0.36f, area.getY());
        cap.lineTo (cx + topW * 0.55f, glassTop);
        cap.lineTo (cx - topW * 0.55f, glassTop);
        cap.closeSubPath();

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff2c36a), cx - topW * 0.55f, 0.0f,
                                                 juce::Colour (0xff6e4c1d), cx + topW * 0.55f, 0.0f, false));
        g.fillPath (cap.createPathWithRoundedCorners (6.0f));
    }

    // Base.
    {
        juce::Path base;
        base.startNewSubPath (cx - botW * 0.485f, glassBot);
        base.lineTo (cx + botW * 0.485f, glassBot);
        base.lineTo (cx + W * 0.5f, area.getBottom());
        base.lineTo (cx - W * 0.5f, area.getBottom());
        base.closeSubPath();

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2c2c33), 0.0f, glassBot,
                                                 juce::Colour (0xff131316), 0.0f, area.getBottom(), false));
        g.fillPath (base.createPathWithRoundedCorners (8.0f));

        g.setColour (AlphaColours::amber);
        g.fillRect (cx - botW * 0.485f, glassBot, botW * 0.97f, 4.0f);

        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.setColour (AlphaColours::amber);
        g.drawText ("ALPHA  LAVA", juce::Rectangle<float> (cx - W * 0.4f, glassBot + baseH * 0.38f, W * 0.8f, 20.0f),
                    juce::Justification::centred);

        g.setFont (juce::FontOptions (10.0f));
        g.setColour (AlphaColours::text.withAlpha (0.55f));
        g.drawText ("AlphaAudio", juce::Rectangle<float> (cx - W * 0.4f, glassBot + baseH * 0.38f + 20.0f, W * 0.8f, 14.0f),
                    juce::Justification::centred);
    }

    // Glass + wax.
    juce::Path glass;
    glass.startNewSubPath (cx - topW * 0.5f, glassTop);
    glass.lineTo (cx + topW * 0.5f, glassTop);
    glass.lineTo (cx + botW * 0.5f, glassBot);
    glass.lineTo (cx - botW * 0.5f, glassBot);
    glass.closeSubPath();
    glass = glass.createPathWithRoundedCorners (10.0f);

    renderField (hue, level, glassH / botW, topW / botW);

    g.saveState();
    g.reduceClipRegion (glass);

    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    const auto glassBox = juce::Rectangle<float> (cx - botW * 0.5f, glassTop, botW, glassH);
    g.drawImage (fieldImage, glassBox);

    // Edge shading for roundness.
    g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.45f), glassBox.getX(), 0.0f,
                                             juce::Colours::transparentBlack, glassBox.getX() + botW * 0.22f, 0.0f, false));
    g.fillRect (glassBox);
    g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.45f), glassBox.getRight(), 0.0f,
                                             juce::Colours::transparentBlack, glassBox.getRight() - botW * 0.22f, 0.0f, false));
    g.fillRect (glassBox);

    // Specular streak following the left edge.
    {
        juce::Path streak;
        streak.startNewSubPath (cx - topW * 0.5f + topW * 0.12f, glassTop + 10.0f);
        streak.lineTo (cx - botW * 0.5f + botW * 0.12f, glassBot - 10.0f);
        g.setColour (juce::Colours::white.withAlpha (0.13f));
        g.strokePath (streak, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    g.restoreState();

    g.setColour (juce::Colours::white.withAlpha (0.20f));
    g.strokePath (glass, juce::PathStrokeType (1.5f));

    // Frequency range markers (bottom of lamp = low band, top = high band).
    g.setFont (juce::FontOptions (10.0f));
    g.setColour (juce::Colours::white.withAlpha (0.50f));

    if (auto* high = processor.apvts.getParameter (ParamIDs::highFreq))
        g.drawText (high->getCurrentValueAsText(),
                    juce::Rectangle<float> (cx - topW * 0.5f + 8.0f, glassTop + 4.0f, 70.0f, 12.0f),
                    juce::Justification::centredLeft);

    if (auto* low = processor.apvts.getParameter (ParamIDs::lowFreq))
        g.drawText (low->getCurrentValueAsText(),
                    juce::Rectangle<float> (cx - botW * 0.5f + 8.0f, glassBot - 16.0f, 70.0f, 12.0f),
                    juce::Justification::centredLeft);
}

//==============================================================================
KnobComponent::KnobComponent (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramID,
                              const juce::String& labelText)
    : attachment (*apvts.getParameter (paramID), slider)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 16);
    addAndMakeVisible (slider);

    label.setText (labelText, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    label.setColour (juce::Label::textColourId, AlphaColours::amber);
    addAndMakeVisible (label);
}

void KnobComponent::resized()
{
    auto r = getLocalBounds();
    label.setBounds (r.removeFromTop (16));
    slider.setBounds (r);
}

//==============================================================================
AlphaLavaAudioProcessorEditor::AlphaLavaAudioProcessorEditor (AlphaLavaAudioProcessor& p)
    : juce::AudioProcessorEditor (&p),
      lamp (p),
      heatKnob (p.apvts, ParamIDs::heat, "HEAT"),
      viscosityKnob (p.apvts, ParamIDs::viscosity, "VISCOSITY"),
      waxKnob (p.apvts, ParamIDs::waxSize, "WAX SIZE"),
      blobsKnob (p.apvts, ParamIDs::blobs, "BLOBS"),
      depthKnob (p.apvts, ParamIDs::depth, "DEPTH"),
      lowKnob (p.apvts, ParamIDs::lowFreq, "LOW FREQ"),
      highKnob (p.apvts, ParamIDs::highFreq, "HIGH FREQ"),
      audioHeatKnob (p.apvts, ParamIDs::audioHeat, "AUDIO HEAT"),
      mixKnob (p.apvts, ParamIDs::mix, "MIX"),
      outputKnob (p.apvts, ParamIDs::output, "OUTPUT"),
      hueKnob (p.apvts, ParamIDs::hue, "WAX COLOUR")
{
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (lamp);

    modeLabel.setText ("MODE", juce::dontSendNotification);
    modeLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    modeLabel.setColour (juce::Label::textColourId, AlphaColours::amber);
    addAndMakeVisible (modeLabel);

    // Items must exist before the attachment is made: it picks the selected index from the item count.
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (p.apvts.getParameter (ParamIDs::mode)))
        modeBox.addItemList (choice->choices, 1);

    addAndMakeVisible (modeBox);
    modeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (p.apvts, ParamIDs::mode, modeBox);

    for (auto* k : { &heatKnob, &viscosityKnob, &waxKnob, &blobsKnob,
                     &depthKnob, &lowKnob, &highKnob, &audioHeatKnob,
                     &mixKnob, &outputKnob, &hueKnob })
        addAndMakeVisible (*k);

    startTimerHz (30);
    setSize (800, 580);   // last: everything above must exist before the first resized()
}

AlphaLavaAudioProcessorEditor::~AlphaLavaAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void AlphaLavaAudioProcessorEditor::timerCallback()
{
    lamp.repaint();
}

void AlphaLavaAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1d1d22), 0.0f, 0.0f,
                                             AlphaColours::charcoal, 0.0f, (float) getHeight(), false));
    g.fillAll();

    const auto panelArea = juce::Rectangle<float> (306.0f, 16.0f, (float) getWidth() - 306.0f - 16.0f, (float) getHeight() - 32.0f);
    g.setColour (AlphaColours::panel);
    g.fillRoundedRectangle (panelArea, 12.0f);
    g.setColour (AlphaColours::panelEdge);
    g.drawRoundedRectangle (panelArea, 12.0f, 1.0f);

    g.setColour (AlphaColours::amber);
    g.setFont (juce::FontOptions (30.0f, juce::Font::bold));
    g.drawText ("ALPHA LAVA", juce::Rectangle<int> (326, 26, 440, 34), juce::Justification::centredLeft);

    g.setColour (AlphaColours::text.withAlpha (0.6f));
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("convective filter  -  the wax is the modulation", juce::Rectangle<int> (328, 58, 440, 16),
                juce::Justification::centredLeft);
}

void AlphaLavaAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    lamp.setBounds (r.removeFromLeft (270));
    r.removeFromLeft (20);
    r = r.reduced (12, 10);

    r.removeFromTop (56);   // title

    auto modeRow = r.removeFromTop (34);
    modeLabel.setBounds (modeRow.removeFromLeft (56));
    modeBox.setBounds (modeRow.removeFromLeft (180).reduced (0, 3));

    r.removeFromTop (6);

    const int rowH = r.getHeight() / 3;

    const auto layoutRow = [] (juce::Rectangle<int> row, std::initializer_list<KnobComponent*> knobs)
    {
        const int cellW = row.getWidth() / 4;

        for (auto* k : knobs)
            k->setBounds (row.removeFromLeft (cellW));
    };

    layoutRow (r.removeFromTop (rowH), { &heatKnob, &viscosityKnob, &waxKnob, &blobsKnob });
    layoutRow (r.removeFromTop (rowH), { &depthKnob, &lowKnob, &highKnob, &audioHeatKnob });
    layoutRow (r.removeFromTop (rowH), { &mixKnob, &outputKnob, &hueKnob });
}
