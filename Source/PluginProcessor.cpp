#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
juce::AudioParameterFloatAttributes percentAttr()
{
    return juce::AudioParameterFloatAttributes()
        .withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; })
        .withValueFromStringFunction ([] (const juce::String& t) { return t.getFloatValue() * 0.01f; });
}

juce::AudioParameterFloatAttributes hzAttr()
{
    return juce::AudioParameterFloatAttributes()
        .withStringFromValueFunction ([] (float v, int)
        {
            return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " kHz"
                                : juce::String (juce::roundToInt (v)) + " Hz";
        })
        .withValueFromStringFunction ([] (const juce::String& t)
        {
            const auto s = t.trim().toLowerCase();
            auto v = s.getFloatValue();
            if (s.contains ("k")) v *= 1000.0f;
            return v;
        });
}

juce::AudioParameterFloatAttributes dbAttr()
{
    return juce::AudioParameterFloatAttributes()
        .withStringFromValueFunction ([] (float v, int) { return juce::String (v, 1) + " dB"; })
        .withValueFromStringFunction ([] (const juce::String& t) { return t.getFloatValue(); });
}
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout AlphaLavaAudioProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { ParamIDs::mode, 1 }, "Mode",
        juce::StringArray { "Bell EQ", "Resonator" }, 0));

    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { ParamIDs::blobs, 1 }, "Blobs", 1, lava::maxBlobs, 4));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::heat, 1 }, "Heat",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.4f, percentAttr()));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::viscosity, 1 }, "Viscosity",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f, percentAttr()));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::waxSize, 1 }, "Wax Size",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f, percentAttr()));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::depth, 1 }, "Depth",
        juce::NormalisableRange<float> (0.0f, 18.0f, 0.1f), 9.0f, dbAttr()));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::lowFreq, 1 }, "Low Freq",
        juce::NormalisableRange<float> (30.0f, 2000.0f, 1.0f, 0.4f), 80.0f, hzAttr()));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::highFreq, 1 }, "High Freq",
        juce::NormalisableRange<float> (2000.0f, 18000.0f, 1.0f, 0.5f), 9000.0f, hzAttr()));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::audioHeat, 1 }, "Audio Heat",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.3f, percentAttr()));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::mix, 1 }, "Mix",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f, percentAttr()));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::output, 1 }, "Output",
        juce::NormalisableRange<float> (-18.0f, 18.0f, 0.1f), 0.0f, dbAttr()));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::hue, 1 }, "Wax Colour",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.02f, percentAttr()));

    return layout;
}

AlphaLavaAudioProcessor::AlphaLavaAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "AlphaLavaState", createLayout())
{
    modeParam      = apvts.getRawParameterValue (ParamIDs::mode);
    blobsParam     = apvts.getRawParameterValue (ParamIDs::blobs);
    heatParam      = apvts.getRawParameterValue (ParamIDs::heat);
    viscosityParam = apvts.getRawParameterValue (ParamIDs::viscosity);
    waxParam       = apvts.getRawParameterValue (ParamIDs::waxSize);
    depthParam     = apvts.getRawParameterValue (ParamIDs::depth);
    lowParam       = apvts.getRawParameterValue (ParamIDs::lowFreq);
    highParam      = apvts.getRawParameterValue (ParamIDs::highFreq);
    audioHeatParam = apvts.getRawParameterValue (ParamIDs::audioHeat);
    mixParam       = apvts.getRawParameterValue (ParamIDs::mix);
    outputParam    = apvts.getRawParameterValue (ParamIDs::output);
}

bool AlphaLavaAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void AlphaLavaAudioProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;

    mixSmooth.reset (sampleRate, 0.02);
    outSmooth.reset (sampleRate, 0.02);
    mixSmooth.setCurrentAndTargetValue (mixParam->load());
    outSmooth.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));

    engine.reset();
    levelEnv = 0.0f;
    lastMode = -1;
    lastCount = 0;
    resetFilterStates();
}

void AlphaLavaAudioProcessor::resetFilterStates() noexcept
{
    for (auto& ch : filters)
        for (auto& f : ch)
            f.resetState();
}

void AlphaLavaAudioProcessor::updateFilters (int mode, int count, int numCh, float wax,
                                             float depthDb, double lowHz, double highHz) noexcept
{
    const double ratio = highHz / lowHz;
    const float resGain = (1.0f + depthDb / 6.0f) / std::sqrt ((float) count);

    for (int b = 0; b < count; ++b)
    {
        const auto& bl = engine.blob (b);
        const float r = engine.radiusOf (b, wax);

        // Height -> frequency (log), size -> bandwidth (bigger blob = lower Q).
        const double yn = juce::jlimit (0.0, 1.0, (double) (bl.y - 0.03f) / 0.94);
        const double freq = lowHz * std::pow (ratio, yn);
        const double q = juce::jmap (juce::jlimit (0.075, 0.30, (double) r), 0.075, 0.30, 6.0, 0.7);

        // How much hotter the wax is than the liquid around it. Hotter = boost (and it rises),
        // cooler = cut (and it sinks): the same quantity that drives the buoyancy.
        const float excess = bl.temp - (1.0f - bl.y);
        const float gainDb = depthDb * juce::jlimit (-1.0f, 1.0f, excess * 2.5f);

        // Left/right position -> how strongly each channel is affected.
        const float pan = juce::jlimit (-1.0f, 1.0f, (bl.x - 0.5f) / 0.3f);
        const float wL = 1.0f - 0.8f * juce::jmax (0.0f, pan);
        const float wR = 1.0f - 0.8f * juce::jmax (0.0f, -pan);

        for (int ch = 0; ch < numCh; ++ch)
        {
            const float w = numCh > 1 ? (ch == 0 ? wL : wR) : 1.0f;
            auto& f = filters[(size_t) ch][(size_t) b];

            if (mode == 0)
            {
                f.setPeaking (currentSampleRate, freq, q, (double) (gainDb * w));
            }
            else
            {
                f.setBandpass (currentSampleRate, freq, q);
                resonatorLevel[(size_t) ch][(size_t) b] = w * (0.25f + 0.75f * bl.temp) * resGain;
            }
        }
    }
}

void AlphaLavaAudioProcessor::publishSnapshot (int count, float wax, float level) noexcept
{
    for (int b = 0; b < count; ++b)
    {
        const auto& bl = engine.blob (b);
        snapshot.x[(size_t) b].store (bl.x, std::memory_order_relaxed);
        snapshot.y[(size_t) b].store (bl.y, std::memory_order_relaxed);
        snapshot.vy[(size_t) b].store (bl.vy, std::memory_order_relaxed);
        snapshot.temp[(size_t) b].store (bl.temp, std::memory_order_relaxed);
        snapshot.radius[(size_t) b].store (engine.radiusOf (b, wax), std::memory_order_relaxed);
    }

    snapshot.count.store (count, std::memory_order_relaxed);
    snapshot.level.store (level, std::memory_order_relaxed);
}

void AlphaLavaAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    const int numSamples = buffer.getNumSamples();

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    if (numCh <= 0 || currentSampleRate <= 0.0)
        return;

    const int mode = (int) modeParam->load();
    const int count = juce::jlimit (1, lava::maxBlobs, (int) blobsParam->load());
    const float wax = waxParam->load();
    const float depthDb = depthParam->load();
    const double lowHz = (double) lowParam->load();
    const double highHz = juce::jmax ((double) highParam->load(), lowHz * 1.5);

    if (mode != lastMode || count > lastCount)
        resetFilterStates();

    lastMode = mode;
    lastCount = count;

    mixSmooth.setTargetValue (mixParam->load());
    outSmooth.setTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));

    lava::PhysicsParams phys;
    phys.heat = heatParam->load();
    phys.viscosity = viscosityParam->load();
    phys.audioHeat = audioHeatParam->load();
    phys.count = count;

    std::array<float*, 2> data { buffer.getWritePointer (0), numCh > 1 ? buffer.getWritePointer (1) : nullptr };

    int pos = 0;

    while (pos < numSamples)
    {
        const int n = juce::jmin (controlBlock, numSamples - pos);
        const float dt = (float) n / (float) currentSampleRate;

        // Input level -> 0..1 (about -54 dB .. 0 dB), with a slow-ish follower so the
        // lamp warms up and cools down like a lamp rather than a meter.
        float sumSq = 0.0f;

        for (int ch = 0; ch < numCh; ++ch)
            for (int i = 0; i < n; ++i)
                sumSq += data[(size_t) ch][pos + i] * data[(size_t) ch][pos + i];

        const float rms = std::sqrt (sumSq / (float) (n * numCh));
        const float target = juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (rms, -80.0f) + 54.0f) / 54.0f);
        const float tau = target > levelEnv ? 0.4f : 1.5f;
        levelEnv += (1.0f - std::exp (-dt / tau)) * (target - levelEnv);

        phys.audioLevel = levelEnv;
        engine.step (dt, phys);
        updateFilters (mode, count, numCh, wax, depthDb, lowHz, highHz);

        for (int i = 0; i < n; ++i)
        {
            const float mix = mixSmooth.getNextValue();
            const float outGain = outSmooth.getNextValue();

            for (int ch = 0; ch < numCh; ++ch)
            {
                float* x = data[(size_t) ch] + pos + i;
                const float dry = *x;
                float wet;

                if (mode == 0)
                {
                    wet = dry;
                    for (int b = 0; b < count; ++b)
                        wet = filters[(size_t) ch][(size_t) b].process (wet);
                }
                else
                {
                    wet = 0.0f;
                    for (int b = 0; b < count; ++b)
                        wet += filters[(size_t) ch][(size_t) b].process (dry) * resonatorLevel[(size_t) ch][(size_t) b];
                }

                *x = (dry * (1.0f - mix) + wet * mix) * outGain;
            }
        }

        pos += n;
    }

    publishSnapshot (count, wax, levelEnv);
}

juce::AudioProcessorEditor* AlphaLavaAudioProcessor::createEditor()
{
    return new AlphaLavaAudioProcessorEditor (*this);
}

void AlphaLavaAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void AlphaLavaAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AlphaLavaAudioProcessor();
}
