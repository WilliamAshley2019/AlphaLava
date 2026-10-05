#pragma once

// AlphaLava - what the lamp "sees": a lock-free tap of the output, and a spectrum/waveform
// analyser for the GUI thread. Std-only (own radix-2 FFT) so it can be tested without JUCE.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace lava
{
// Audio thread writes, GUI thread reads. Relaxed atomics keep it race-free without locks;
// a slightly torn frame between writer and reader is harmless for drawing.
class SpectrumTap
{
public:
    static constexpr int ringSize = 8192;
    static constexpr std::uint32_t ringMask = (std::uint32_t) ringSize - 1u;

    void pushBlock (const float* l, const float* r, int n) noexcept
    {
        const auto w = writeIndex.load (std::memory_order_relaxed);

        for (int i = 0; i < n; ++i)
            ring[(w + (std::uint32_t) i) & ringMask].store (r != nullptr ? 0.5f * (l[i] + r[i]) : l[i],
                                                           std::memory_order_relaxed);

        writeIndex.store (w + (std::uint32_t) n, std::memory_order_release);
    }

    void copyLatest (float* dst, int n) const noexcept
    {
        const auto w = writeIndex.load (std::memory_order_acquire);

        for (int i = 0; i < n; ++i)
            dst[i] = ring[(w - (std::uint32_t) n + (std::uint32_t) i) & ringMask].load (std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<float>, ringSize> ring {};
    std::atomic<std::uint32_t> writeIndex { 0 };
};

//==============================================================================
class LavaSpectrum
{
public:
    static constexpr int fftSize = 2048;
    static constexpr int rows = 48;     // spectrum rows up the lamp
    static constexpr int waveN = 96;    // points in the waveform ribbon

    std::array<float, rows> level {};   // 0..1 per row, bottom (low) -> top (high)
    std::array<float, waveN> wave {};   // -1..1, oldest -> newest

    LavaSpectrum()
        : re ((size_t) fftSize), im ((size_t) fftSize), window ((size_t) fftSize),
          mags ((size_t) fftSize / 2 + 2), cosT ((size_t) fftSize / 2), sinT ((size_t) fftSize / 2),
          samples ((size_t) fftSize), rev ((size_t) fftSize)
    {
        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (6.283185307179586f * (float) i / (float) (fftSize - 1));

        for (int i = 0; i < fftSize / 2; ++i)
        {
            cosT[(size_t) i] = std::cos (6.283185307179586f * (float) i / (float) fftSize);
            sinT[(size_t) i] = std::sin (6.283185307179586f * (float) i / (float) fftSize);
        }

        int bits = 0;
        while ((1 << bits) < fftSize) ++bits;

        for (int i = 0; i < fftSize; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[(size_t) i] = r;
        }
    }

    // lowHz..highHz is mapped onto the rows with the same log mapping the blobs use,
    // so a blob sitting on a row is literally filtering what that row shows.
    void analyse (const SpectrumTap& tap, double sampleRate, double lowHz, double highHz)
    {
        tap.copyLatest (samples.data(), fftSize);

        for (int i = 0; i < fftSize; ++i)
        {
            re[(size_t) i] = samples[(size_t) rev[(size_t) i]] * window[(size_t) rev[(size_t) i]];
            im[(size_t) i] = 0.0f;
        }

        fft();

        for (int k = 0; k <= fftSize / 2 + 1; ++k)
        {
            const size_t kk = (size_t) std::min (k, fftSize / 2);
            mags[(size_t) k] = std::sqrt (re[kk] * re[kk] + im[kk] * im[kk]);
        }

        const double ratio = highHz / std::max (lowHz, 1.0);
        const double binHz = sampleRate / (double) fftSize;

        for (int r = 0; r < rows; ++r)
        {
            const double f0 = lowHz * std::pow (ratio, (double) r / rows);
            const double f1 = lowHz * std::pow (ratio, (double) (r + 1) / rows);
            const double b0 = f0 / binHz, b1 = f1 / binHz;

            float mag;

            if (b1 - b0 < 1.0)
            {
                const double c = std::clamp (0.5 * (b0 + b1), 1.0, (double) fftSize / 2 - 2.0);
                const int i = (int) c;
                const float fr = (float) (c - i);
                mag = mags[(size_t) i] * (1.0f - fr) + mags[(size_t) i + 1] * fr;
            }
            else
            {
                const int i0 = std::clamp ((int) std::ceil (b0), 1, fftSize / 2 - 1);
                const int i1 = std::clamp ((int) std::floor (b1), i0, fftSize / 2 - 1);
                mag = 0.0f;
                for (int i = i0; i <= i1; ++i) mag = std::max (mag, mags[(size_t) i]);
            }

            // Full-scale sine through a Hann window lands at about N/4. Tilt +3 dB/oct so a typical
            // mix doesn't just light up the bottom of the lamp.
            const double fc = std::sqrt (f0 * f1);
            const float db = 20.0f * std::log10 (mag / ((float) fftSize * 0.25f) + 1.0e-9f)
                             + 3.0f * (float) std::log2 (fc / 1000.0);
            const float v = std::clamp ((db + 78.0f) / 72.0f, 0.0f, 1.0f);

            auto& l = level[(size_t) r];
            l = v > l ? l + (v - l) * 0.6f : std::max (v, l * 0.90f);
        }

        // Waveform ribbon: the newest ~1024 samples, auto-gained so quiet material still shows.
        constexpr int span = 1024;
        float peak = 0.05f;
        for (int i = fftSize - span; i < fftSize; ++i)
            peak = std::max (peak, std::abs (samples[(size_t) i]));

        wavePeak += (std::max (peak, 0.1f) - wavePeak) * (peak > wavePeak ? 0.5f : 0.05f);

        for (int i = 0; i < waveN; ++i)
        {
            const int s = fftSize - span + (i * span) / waveN + span / (2 * waveN);
            wave[(size_t) i] = std::clamp (samples[(size_t) s] / wavePeak, -1.0f, 1.0f);
        }
    }

private:
    void fft() noexcept
    {
        // Input is already in bit-reversed order (done when filling re/im).
        for (int len = 2; len <= fftSize; len <<= 1)
        {
            const int half = len / 2;
            const int step = fftSize / len;

            for (int i = 0; i < fftSize; i += len)
                for (int k = 0; k < half; ++k)
                {
                    const float wr = cosT[(size_t) (k * step)];
                    const float wi = -sinT[(size_t) (k * step)];
                    const size_t a = (size_t) (i + k), b = (size_t) (i + k + half);
                    const float tr = re[b] * wr - im[b] * wi;
                    const float ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr;  im[b] = im[a] - ti;
                    re[a] += tr;         im[a] += ti;
                }
        }
    }

    std::vector<float> re, im, window, mags, cosT, sinT, samples;
    std::vector<int> rev;
    float wavePeak = 0.1f;
};
} // namespace lava
