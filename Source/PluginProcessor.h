#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>

#include "LavaDsp.h"
#include "LavaEngine.h"
#include "LavaHaze.h"
#include "LavaScope.h"

namespace ParamIDs
{
inline const juce::String mode       { "mode" };
inline const juce::String blobs      { "blobs" };
inline const juce::String heat       { "heat" };
inline const juce::String viscosity  { "viscosity" };
inline const juce::String waxSize    { "waxSize" };
inline const juce::String depth      { "depth" };
inline const juce::String lowFreq    { "lowFreq" };
inline const juce::String highFreq   { "highFreq" };
inline const juce::String audioHeat  { "audioHeat" };
inline const juce::String mix        { "mix" };
inline const juce::String output     { "output" };
inline const juce::String hue        { "hue" };
inline const juce::String power      { "power" };
inline const juce::String warmMinutes{ "warmMinutes" };
inline const juce::String tube       { "tube" };
inline const juce::String trip       { "trip" };
inline const juce::String echoTime   { "echoTime" };
inline const juce::String echoFeedback { "echoFeedback" };
inline const juce::String echoMix    { "echoMix" };
} // namespace ParamIDs

class AlphaLavaAudioProcessor : public juce::AudioProcessor
{
public:
    AlphaLavaAudioProcessor();
    ~AlphaLavaAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "AlphaLava"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    const lava::BlobSnapshot& getSnapshot() const noexcept { return snapshot; }
    const lava::SpectrumTap& getSpectrumTap() const noexcept { return spectrumTap; }

    // GUI buttons: 1 = slow-mo, 2 = repeat. Picked up by the audio thread at the next control block.
    void requestEvent (int kind) noexcept { manualEvent.store (kind); }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    void resetFilterStates() noexcept;
    void updateFilters (int mode, int count, int numCh, float wax, float depthDb, double lowHz, double highHz) noexcept;
    void publishSnapshot (int count, float wax, float level) noexcept;
    void resetEffects() noexcept;

    static constexpr int controlBlock = 32;

    float nextRand() noexcept
    {
        rngState ^= rngState << 13;
        rngState ^= rngState >> 17;
        rngState ^= rngState << 5;
        return (float) (rngState >> 8) * (1.0f / 16777216.0f);
    }

    lava::LavaEngine engine;
    lava::BlobSnapshot snapshot;
    lava::SpectrumTap spectrumTap;
    lava::TubeStage tube;
    lava::LavaHaze haze;

    // [channel][blob]
    std::array<std::array<lava::Biquad, lava::maxBlobs>, 2> filters;
    std::array<std::array<float, lava::maxBlobs>, 2> resonatorLevel {};

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth, outSmooth;

    std::atomic<float>* modeParam = nullptr;
    std::atomic<float>* blobsParam = nullptr;
    std::atomic<float>* heatParam = nullptr;
    std::atomic<float>* viscosityParam = nullptr;
    std::atomic<float>* waxParam = nullptr;
    std::atomic<float>* depthParam = nullptr;
    std::atomic<float>* lowParam = nullptr;
    std::atomic<float>* highParam = nullptr;
    std::atomic<float>* audioHeatParam = nullptr;
    std::atomic<float>* mixParam = nullptr;
    std::atomic<float>* outputParam = nullptr;
    std::atomic<float>* powerParam = nullptr;
    std::atomic<float>* warmParam = nullptr;
    std::atomic<float>* tubeParam = nullptr;
    std::atomic<float>* tripParam = nullptr;
    std::atomic<float>* echoTimeParam = nullptr;
    std::atomic<float>* echoFbParam = nullptr;
    std::atomic<float>* echoMixParam = nullptr;

    std::atomic<int> manualEvent { 0 };
    std::uint32_t rngState = 0x9e3779b9u;

    double currentSampleRate = 44100.0;
    float warmth = 0.0f;       // 0..1, climbs while the lamp is on, falls while it is off
    float bulb = 0.0f;         // 0..1 bulb brightness
    bool wasBypassed = true;
    float levelEnv = 0.0f;
    int lastMode = -1;
    int lastCount = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AlphaLavaAudioProcessor)
};
