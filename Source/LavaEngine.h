#pragma once

// AlphaLava - convection simulation.
// Std-only on purpose: no JUCE types, no allocation, safe to run on the audio thread.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace lava
{
inline constexpr int maxBlobs = 6;

// Blob coordinates: x in [0..1] across the lamp, y in [0..1] bottom -> top.
struct Blob
{
    float x = 0.5f, y = 0.1f;
    float vx = 0.0f, vy = 0.0f;
    float temp = 0.8f;       // 0 = cold, 1 = hot
    float size = 1.0f;       // per-blob size multiplier
    float home = 0.5f;       // preferred column
    float buoyScale = 1.0f;  // per-blob personality
    float rateScale = 1.0f;  // per-blob thermal inertia
};

struct PhysicsParams
{
    float heat = 0.5f;        // 0..1 base lamp temperature (time scale)
    float viscosity = 0.5f;   // 0..1 drag
    float audioLevel = 0.0f;  // 0..1 smoothed input level
    float audioHeat = 0.0f;   // 0..1 how much audio level heats the lamp
    float warmth = 1.0f;      // 0..1 how warmed-up the lamp is (cold wax sits in the pool)
    int count = 4;
};

// Written by the audio thread, read by the GUI. Plain relaxed atomics: a torn
// frame between blobs is harmless for drawing.
struct BlobSnapshot
{
    std::array<std::atomic<float>, maxBlobs> x {}, y {}, vy {}, temp {}, radius {};
    std::atomic<int> count { 0 };
    std::atomic<float> level { 0.0f };
    std::atomic<float> warmth { 0.0f };     // 0..1 lamp body temperature
    std::atomic<float> bulb { 0.0f };       // 0..1 bulb brightness (comes on fast, fades slowly)
    std::atomic<int> powerOn { 1 };
    std::atomic<int> eventKind { 0 };       // 0 none, 1 slow-mo, 2 repeat
};

class LavaEngine
{
public:
    LavaEngine() { reset(); }

    void reset (std::uint32_t seed = 0x1a2b3c4dU) noexcept
    {
        rng = seed != 0 ? seed : 1u;

        for (int i = 0; i < maxBlobs; ++i)
        {
            auto& b = blobs[(size_t) i];
            const auto fi = (float) i;

            b.y = 0.05f + 0.85f * frac (0.35f + 0.618034f * fi);
            b.temp = clamp01 ((1.0f - b.y) + 0.1f * (rnd() - 0.5f));
            b.vy = 0.0f;
            b.vx = 0.0f;
            b.home = 0.3f + 0.4f * frac (0.2f + 0.754878f * fi);
            b.x = b.home;
            b.size = 0.80f + 0.45f * frac (0.5f + 0.569840f * fi);
            b.buoyScale = 0.85f + 0.30f * frac (0.1f + 0.431270f * fi);
            b.rateScale = 0.80f + 0.40f * frac (0.7f + 0.325980f * fi);
        }
    }

    void step (float dt, const PhysicsParams& p) noexcept
    {
        const float w = clamp01 (p.warmth);
        const float heatEff = clamp01 (p.heat + p.audioHeat * clamp01 (p.audioLevel));

        // Slow and syrupy at the bottom of the range, brisk at the top. A cold lamp is sluggish too.
        const float h = dt * (0.1f + 0.6f * heatEff + 0.9f * heatEff * heatEff * heatEff) * (0.35f + 0.65f * w);
        const float drag = 0.6f + 1.4f * clamp01 (p.viscosity);
        const int n = std::min (p.count, maxBlobs);

        for (int i = 0; i < n; ++i)
        {
            auto& b = blobs[(size_t) i];

            // Each blob lets go of the pool at a different warmth, so they lift off one by one.
            const float lift = smooth01 ((w - (0.18f + 0.08f * (float) i)) / 0.25f);

            // Thermal: wax chases the ambient temperature (hot at the bulb, cold at the top).
            // The lag is what makes it oscillate instead of settling.
            const float rate = 0.4f * b.rateScale;
            b.temp += h * rate * ((1.0f - b.y) - b.temp) + 4.0f * h * (rnd() - 0.5f);
            b.temp = clamp01 (b.temp);

            // Buoyancy vs drag. Without enough warmth, gravity wins and the wax pools.
            const float buoy = 6.0f * b.buoyScale * lift;
            const float gravity = 3.0f * (1.0f - lift);
            b.vy += h * (buoy * (b.temp - 0.5f) - gravity - drag * b.vy);
            b.y += h * b.vy;

            if (b.y < 0.03f)
            {
                b.y = 0.03f;
                if (b.vy < 0.0f) { if (b.vy < -0.25f) ++bottomHits; b.vy *= -0.2f; }
            }

            if (b.y > 0.97f)
            {
                b.y = 0.97f;
                if (b.vy > 0.0f) { if (b.vy > 0.25f) ++topHits; b.vy *= -0.2f; }
            }

            // Sideways: drift home, avoid neighbours at similar height, wander a little.
            float fx = (b.home - b.x) * 1.5f;

            for (int j = 0; j < n; ++j)
            {
                if (j == i) continue;
                const auto& o = blobs[(size_t) j];
                const float dx = b.x - o.x, dy = b.y - o.y;
                const float nearY = std::max (0.0f, 1.0f - std::abs (dy) / 0.3f);
                const float nearX = std::max (0.0f, 1.0f - std::abs (dx) / 0.35f);
                fx += (dx >= 0.0f ? 1.2f : -1.2f) * nearY * nearX;
            }

            fx += 30.0f * (rnd() - 0.5f);
            b.vx += h * (fx - 3.0f * b.vx);
            b.x = std::clamp (b.x + h * b.vx, 0.2f, 0.8f);
        }
    }

    // Wall hits since the last call: a blob reaching the top ("pop") or diving into the pool.
    void consumeEvents (int& top, int& bottom) noexcept
    {
        top = topHits;
        bottom = bottomHits;
        topHits = bottomHits = 0;
    }

    const Blob& blob (int i) const noexcept { return blobs[(size_t) i]; }

    // Radius in lamp-width units.
    float radiusOf (int i, float waxSize) const noexcept
    {
        return (0.10f + 0.14f * clamp01 (waxSize)) * blobs[(size_t) i].size;
    }

private:
    static float clamp01 (float v) noexcept { return std::min (1.0f, std::max (0.0f, v)); }
    static float frac (float v) noexcept { return v - std::floor (v); }
    static float smooth01 (float x) noexcept { const float t = clamp01 (x); return t * t * (3.0f - 2.0f * t); }

    float rnd() noexcept   // xorshift32 -> [0,1)
    {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return (float) (rng >> 8) * (1.0f / 16777216.0f);
    }

    std::array<Blob, maxBlobs> blobs {};
    std::uint32_t rng = 1u;
    int topHits = 0, bottomHits = 0;
};
} // namespace lava
