#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// Per-area colour grading ("trigger_grade" brush volumes): how much of a volume's grade applies at
// a camera position. Header-only, so iw4r_mapload.dll (no core library) and the editor share it.
namespace gradevol {

// Outward plane: a point p is inside when n . p <= d.
struct Plane {
    float n[3];
    float d;
};
using Brush = std::vector<Plane>;

// How far `p` is inside the convex brush (negative: outside, by at least that much).
inline float insideDepth(const Brush& b, const float p[3]) {
    float depth = 1e30f;
    for (const Plane& pl : b) depth = std::min(depth, pl.d - (pl.n[0] * p[0] + pl.n[1] * p[1] + pl.n[2] * p[2]));
    return b.empty() ? -1e30f : depth;
}

// The volume's target weight (0..1): full once `blend` units inside its deepest brush.
inline float targetWeight(const std::vector<Brush>& brushes, float blend, const float p[3]) {
    float depth = -1e30f;
    for (const Brush& b : brushes) depth = std::max(depth, insideDepth(b, p));
    if (depth < 0) return 0;
    return blend > 0 ? std::min(1.0f, depth / blend) : 1.0f;
}

// Eases a weight towards its target over `seconds` (0 = at once).
inline float approach(float w, float target, float seconds, float dt) {
    if (seconds <= 0) return target;
    const float step = dt / seconds;
    return w < target ? std::min(target, w + step) : std::max(target, w - step);
}

// out = lerp(out, lut, w) per byte (RGB8 tables of the same size).
inline void blendInto(std::vector<uint8_t>& out, const std::vector<uint8_t>& lut, float w) {
    if (w <= 0 || out.size() != lut.size()) return;
    const int wi = static_cast<int>(w * 256 + 0.5f);
    for (size_t i = 0; i < out.size(); i++) out[i] = static_cast<uint8_t>(out[i] + (((lut[i] - out[i]) * wi) >> 8));
}

}  // namespace gradevol
