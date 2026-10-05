#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>

#include "PluginProcessor.h"

// AlphaAudio house look: dark charcoal with amber-gold.
namespace AlphaColours
{
inline const juce::Colour charcoal  { 0xff17171b };
inline const juce::Colour panel     { 0xff222228 };
inline const juce::Colour panelEdge { 0xff34343c };
inline const juce::Colour amber     { 0xffe8a33d };
inline const juce::Colour amberDim  { 0xff8a6224 };
inline const juce::Colour text      { 0xffd9d4c8 };
} // namespace AlphaColours

class AlphaLookAndFeel : public juce::LookAndFeel_V4
{
public:
    AlphaLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider&) override;
};

// The lamp itself. Reads the audio thread's blob snapshot and draws it as metaballs.
class LampComponent : public juce::Component
{
public:
    explicit LampComponent (AlphaLavaAudioProcessor& p);
    void paint (juce::Graphics&) override;

private:
    void renderField (float waxHue, float level, float aspect, float widthRatioTopToBottom);

    AlphaLavaAudioProcessor& processor;
    juce::Image fieldImage;

    static constexpr int fieldW = 56;
    static constexpr int fieldH = 128;
};

class KnobComponent : public juce::Component
{
public:
    KnobComponent (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramID, const juce::String& labelText);
    void resized() override;

private:
    juce::Label label;
    juce::Slider slider;
    juce::SliderParameterAttachment attachment;
};

class AlphaLavaAudioProcessorEditor : public juce::AudioProcessorEditor,
                                      private juce::Timer
{
public:
    explicit AlphaLavaAudioProcessorEditor (AlphaLavaAudioProcessor&);
    ~AlphaLavaAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    AlphaLookAndFeel lookAndFeel;

    LampComponent lamp;

    juce::Label modeLabel;
    juce::ComboBox modeBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAttachment;  // made after items exist

    KnobComponent heatKnob, viscosityKnob, waxKnob, blobsKnob;
    KnobComponent depthKnob, lowKnob, highKnob, audioHeatKnob;
    KnobComponent mixKnob, outputKnob, hueKnob;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AlphaLavaAudioProcessorEditor)
};
