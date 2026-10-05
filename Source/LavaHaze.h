#pragma once

// AlphaLava - the 70s smoke-filled-room stuff. Std-only; all allocation happens in prepare().

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace lava
{
inline float hermite4 (float xm1, float x0, float x1, float x2, float t) noexcept
{
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

//==============================================================================
// The bulb/tube: gentle asymmetric saturation that fills out as the lamp warms,
// and a cold-glass dullness that opens up with temperature.
class TubeStage
{
public:
    void prepare (double sr) noexcept { sampleRate = sr; reset(); }

    void reset() noexcept
    {
        for (auto& s : state) s = {};
    }

    // tubeAmount: the Tube knob (0..1). warmth: 0 = stone cold .. 1 = fully warm.
    void setAmount (float tubeAmount, float warmth) noexcept
    {
        active = tubeAmount > 0.001f;

        const float amt = tubeAmount * warmth;
        drive = 1.0f + 5.0f * amt;
        comp = 1.0f / std::sqrt (drive);

        const float coldTerm = std::min (1.0f, tubeAmount * 2.0f * (1.0f - warmth));
        const double fc = std::min (20000.0 * std::pow (0.15, (double) coldTerm), sampleRate * 0.45);
        lpCoef = (float) (1.0 - std::exp (-6.283185307179586 * fc / sampleRate));
    }

    float process (int ch, float x) noexcept
    {
        if (! active)
            return x;

        auto& s = state[(size_t) ch];
        s.lp += lpCoef * (x - s.lp);

        const float y = (std::tanh (drive * s.lp + bias) - biasTanh) * comp;
        const float out = y - s.dcX + 0.995f * s.dcY;   // the bias makes DC; take it back out
        s.dcX = y;
        s.dcY = out;
        return out;
    }

private:
    struct State { float lp = 0.0f, dcX = 0.0f, dcY = 0.0f; };

    std::array<State, 2> state {};
    double sampleRate = 44100.0;
    float drive = 1.0f, comp = 1.0f, lpCoef = 1.0f;
    bool active = false;

    static constexpr float bias = 0.12f;
    inline static const float biasTanh = std::tanh (0.12f);
};

//==============================================================================
class LavaHaze
{
public:
    enum class Kind : int { none = 0, slowMo = 1, repeat = 2 };

    void prepare (double sr, int channels)
    {
        sampleRate = sr;
        numCh = std::clamp (channels, 1, 2);

        tapeSize = nextPow2 ((int) (4.5 * sr));
        echoSize = nextPow2 ((int) (2.6 * sr));
        tapeMask = tapeSize - 1;
        echoMask = echoSize - 1;

        for (size_t c = 0; c < 2; ++c)
        {
            tape[c].assign ((size_t) tapeSize, 0.0f);
            echo[c].assign ((size_t) echoSize, 0.0f);
        }

        smoothSlow = 1.0f - std::exp (-1.0f / (0.08f * (float) sr));
        smoothFast = 1.0f - std::exp (-1.0f / (0.02f * (float) sr));
        fbLpCoef = (float) (1.0 - std::exp (-6.283185307179586 * 3200.0 / sr));
        fbHpCoef = (float) (1.0 - std::exp (-6.283185307179586 * 150.0 / sr));

        reset();
        delaySm = targetDelay = 0.34 * sr;
    }

    void reset() noexcept
    {
        for (size_t c = 0; c < 2; ++c)
        {
            std::fill (tape[c].begin(), tape[c].end(), 0.0f);
            std::fill (echo[c].begin(), echo[c].end(), 0.0f);
            echoLp[c] = echoHp[c] = 0.0f;
        }

        kind = Kind::none;
        cooldown = 0;
        evSample = 0;
        mixSm = fbSm = 0.0f;
    }

    void setEcho (float timeMs, float feedback, float mix, float trip) noexcept
    {
        targetDelay = (double) timeMs * 0.001 * sampleRate;
        fbTarget = std::clamp (feedback, 0.0f, 0.88f);
        mixTarget = mix;
        tripAmount = trip;
    }

    Kind currentKind() const noexcept { return kind; }

    // a, b: caller-supplied randoms in [0,1). force ignores the cooldown (manual buttons).
    bool trigger (Kind k, float a, float b, bool force = false) noexcept
    {
        if (kind != Kind::none || (! force && cooldown > 0) || tapeSize == 0)
            return false;

        evSample = 0;

        if (k == Kind::slowMo)
        {
            depthSamples = (0.25 + 0.45 * (double) a) * sampleRate;
            slowDown = (std::int64_t) ((1.2 + 1.3 * (double) b) * sampleRate);
            holdLen = (std::int64_t) ((0.25 + 0.35 * (double) a) * sampleRate);
            xfadeOut = (std::int64_t) (0.08 * sampleRate);
        }
        else if (k == Kind::repeat)
        {
            loopLen = std::max<std::int64_t> (64, (std::int64_t) ((0.09 + 0.26 * (double) a) * sampleRate));
            loops = std::clamp (3 + (int) (b * 5.0f), 3, 7);
            loopStart = wp - loopLen;
            entryLen = std::max<std::int64_t> (16, (std::int64_t) (0.006 * sampleRate));
            xfadeOut = (std::int64_t) (0.03 * sampleRate);
            seam = std::min<std::int64_t> (256, loopLen / 4);
        }
        else
            return false;

        kind = k;
        return true;
    }

    // In place. For a mono instance, r is ignored (pass anything).
    void processSample (float& l, float& r) noexcept
    {
        const std::int64_t w = wp;
        const bool stereo = numCh > 1;

        tape[0][(size_t) (w & tapeMask)] = l;
        if (stereo) tape[1][(size_t) (w & tapeMask)] = r;

        // ---- time-warp events -------------------------------------------------------------
        float el = l, er = r;
        float g = 0.0f;

        if (kind == Kind::slowMo)
        {
            const double u = std::min (1.0, (double) evSample / (double) std::max<std::int64_t> (1, slowDown));
            const double d = 2.0 + depthSamples * (u * u * (3.0 - 2.0 * u));

            el = readTape (0, w, d);
            if (stereo) er = readTape (1, w, d);

            const std::int64_t exitStart = slowDown + holdLen;
            g = evSample < exitStart ? 1.0f : 1.0f - (float) (evSample - exitStart) / (float) std::max<std::int64_t> (1, xfadeOut);
            ++evSample;

            if (g <= 0.0f) endEvent();
        }
        else if (kind == Kind::repeat)
        {
            const std::int64_t p = evSample % loopLen;

            const auto loopRead = [this, p] (size_t c)
            {
                float a = tape[c][(size_t) ((loopStart + p) & tapeMask)];

                if (p >= loopLen - seam)   // crossfade the seam so the loop point doesn't click
                {
                    const float t = (float) (p - (loopLen - seam)) / (float) seam;
                    const float bb = tape[c][(size_t) ((loopStart + p - loopLen) & tapeMask)];
                    a = a * (1.0f - t) + bb * t;
                }

                return a;
            };

            el = loopRead (0);
            if (stereo) er = loopRead (1);

            const std::int64_t total = loopLen * loops;
            const float gIn = std::min (1.0f, (float) evSample / (float) entryLen);
            const float gOut = evSample < total ? 1.0f : 1.0f - (float) (evSample - total) / (float) std::max<std::int64_t> (1, xfadeOut);
            g = std::min (gIn, gOut);
            ++evSample;

            if (gOut <= 0.0f) endEvent();
        }
        else if (cooldown > 0)
            --cooldown;

        if (! stereo) er = el;

        float in[2] = { l * (1.0f - g) + el * g, (stereo ? r : l) * (1.0f - g) + er * g };

        // ---- wobbly tape echo -------------------------------------------------------------
        delaySm += (targetDelay - delaySm) * (double) smoothSlow;
        fbSm += (fbTarget - fbSm) * smoothFast;
        mixSm += (mixTarget - mixSm) * smoothFast;

        lfoPhase += 6.283185307179586 * 0.37 / sampleRate;
        if (lfoPhase > 6.283185307179586) lfoPhase -= 6.283185307179586;

        const double wob = (0.0005 + 0.005 * (double) tripAmount) * sampleRate;
        float delayed[2] = { 0.0f, 0.0f }, fb[2] = { 0.0f, 0.0f };

        for (int c = 0; c < numCh; ++c)
        {
            const double base = delaySm * (c == 0 ? 1.0 : 1.18);
            const double mod = wob * (std::sin (lfoPhase + 1.7 * c) + 0.5 * std::sin (lfoPhase * 0.43 + (double) c));
            const double d = std::clamp (base + mod, 3.0, (double) echoSize - 8.0);

            delayed[c] = readEcho ((size_t) c, w, d);

            // Dub-style feedback path: darker and thinner each trip round, then soft-limited.
            echoLp[c] += fbLpCoef * (delayed[c] - echoLp[c]);
            echoHp[c] += fbHpCoef * (echoLp[c] - echoHp[c]);
            fb[c] = std::tanh (echoLp[c] - echoHp[c]);
        }

        const float self = stereo ? 0.7f : 1.0f;
        const float cross = stereo ? 0.3f : 0.0f;

        for (int c = 0; c < numCh; ++c)
            echo[(size_t) c][(size_t) (w & echoMask)] = in[c] + fbSm * (self * fb[c] + cross * fb[1 - c]);

        l = in[0] + mixSm * delayed[0];
        if (stereo) r = in[1] + mixSm * delayed[1];

        ++wp;
    }

private:
    static int nextPow2 (int v) noexcept
    {
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    void endEvent() noexcept
    {
        kind = Kind::none;
        cooldown = (std::int64_t) (3.0 * sampleRate);
    }

    float readTape (size_t c, std::int64_t w, double d) const noexcept
    {
        const double pos = (double) w - d;
        const auto i0 = (std::int64_t) std::floor (pos);
        const float t = (float) (pos - (double) i0);
        const auto& b = tape[c];

        return hermite4 (b[(size_t) ((i0 - 1) & tapeMask)], b[(size_t) (i0 & tapeMask)],
                         b[(size_t) ((i0 + 1) & tapeMask)], b[(size_t) ((i0 + 2) & tapeMask)], t);
    }

    float readEcho (size_t c, std::int64_t w, double d) const noexcept
    {
        const double pos = (double) w - d;
        const auto i0 = (std::int64_t) std::floor (pos);
        const float t = (float) (pos - (double) i0);
        const auto& b = echo[c];

        return hermite4 (b[(size_t) ((i0 - 1) & echoMask)], b[(size_t) (i0 & echoMask)],
                         b[(size_t) ((i0 + 1) & echoMask)], b[(size_t) ((i0 + 2) & echoMask)], t);
    }

    std::array<std::vector<float>, 2> tape, echo;
    std::int64_t tapeSize = 0, echoSize = 0, tapeMask = 0, echoMask = 0;
    std::int64_t wp = 0;

    double sampleRate = 44100.0;
    int numCh = 2;

    Kind kind = Kind::none;
    std::int64_t evSample = 0, cooldown = 0;

    double depthSamples = 0.0;
    std::int64_t slowDown = 1, holdLen = 0, xfadeOut = 1;

    std::int64_t loopLen = 1, loopStart = 0, entryLen = 1, seam = 1;
    int loops = 3;

    double delaySm = 0.0, targetDelay = 0.0, lfoPhase = 0.0;
    float fbSm = 0.0f, fbTarget = 0.0f, mixSm = 0.0f, mixTarget = 0.0f, tripAmount = 0.0f;
    float smoothSlow = 0.001f, smoothFast = 0.01f, fbLpCoef = 0.3f, fbHpCoef = 0.02f;
    std::array<float, 2> echoLp {}, echoHp {};
};
} // namespace lava
