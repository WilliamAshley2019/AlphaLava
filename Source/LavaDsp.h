#pragma once

#include <algorithm>
#include <cmath>

namespace lava
{
// Transposed direct form II biquad. Coefficients are retargeted every control block (32 samples),
// which is far faster than the lamp moves, so no coefficient crossfading is needed.
struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    inline float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void resetState() noexcept { z1 = z2 = 0.0f; }

    void setPeaking (double sr, double freq, double q, double gainDb) noexcept
    {
        const double f = std::clamp (freq, 20.0, sr * 0.45);
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w0 = 6.283185307179586 * f / sr;
        const double alpha = std::sin (w0) / (2.0 * std::max (q, 0.1));
        const double c = std::cos (w0);
        const double a0 = 1.0 + alpha / A;

        b0 = (float) ((1.0 + alpha * A) / a0);
        b1 = (float) ((-2.0 * c) / a0);
        b2 = (float) ((1.0 - alpha * A) / a0);
        a1 = (float) ((-2.0 * c) / a0);
        a2 = (float) ((1.0 - alpha / A) / a0);
    }

    // Constant 0 dB peak-gain bandpass.
    void setBandpass (double sr, double freq, double q) noexcept
    {
        const double f = std::clamp (freq, 20.0, sr * 0.45);
        const double w0 = 6.283185307179586 * f / sr;
        const double alpha = std::sin (w0) / (2.0 * std::max (q, 0.1));
        const double c = std::cos (w0);
        const double a0 = 1.0 + alpha;

        b0 = (float) (alpha / a0);
        b1 = 0.0f;
        b2 = (float) (-alpha / a0);
        a1 = (float) ((-2.0 * c) / a0);
        a2 = (float) ((1.0 - alpha) / a0);
    }
};
} // namespace lava
