#include "light/colorgrade.h"

#include <algorithm>
#include <cmath>

#include "model/imageio.h"

namespace light {

bool loadStripLut(const std::string& path, GradeLut& out, std::string& error) {
    imageio::Rgba img;
    if (!imageio::load(path, img, error)) return false;
    const int n = img.height;
    if (n < 2 || n > 64 || img.width != n * n) {
        error = path + ": a LUT strip must be (size*size) x size pixels, e.g. 256x16 or 1024x32 (got " + std::to_string(img.width) + "x" +
                std::to_string(img.height) + ")";
        return false;
    }
    out.size = n;
    out.rgb.assign(static_cast<size_t>(n) * n * n * 3, 0);
    for (int b = 0; b < n; b++)
        for (int g = 0; g < n; g++)
            for (int r = 0; r < n; r++) {
                const uint8_t* px = &img.pixels[(static_cast<size_t>(g) * img.width + b * n + r) * 4];
                uint8_t* o = &out.rgb[((static_cast<size_t>(b) * n + g) * n + r) * 3];
                o[0] = px[0], o[1] = px[1], o[2] = px[2];
            }
    return true;
}

bool saveNeutralStripLut(const std::string& path, int n, std::string& error) {
    n = std::clamp(n, 2, 64);
    imageio::Rgba img;
    img.width = n * n;
    img.height = n;
    img.pixels.assign(static_cast<size_t>(img.width) * n * 4, 255);
    auto v = [&](int i) { return static_cast<uint8_t>(std::lround(i * 255.0 / (n - 1))); };
    for (int b = 0; b < n; b++)
        for (int g = 0; g < n; g++)
            for (int r = 0; r < n; r++) {
                uint8_t* px = &img.pixels[(static_cast<size_t>(g) * img.width + b * n + r) * 4];
                px[0] = v(r), px[1] = v(g), px[2] = v(b);
            }
    return imageio::savePng(path, img, error);
}

namespace {

// Trilinear lookup in a table (input 0..1).
void sampleLut(const GradeLut& t, const double in[3], double out[3]) {
    const int n = t.size;
    double f[3];
    int i0[3], i1[3];
    for (int k = 0; k < 3; k++) {
        const double x = std::clamp(in[k], 0.0, 1.0) * (n - 1);
        i0[k] = std::min(static_cast<int>(x), n - 2);
        i1[k] = i0[k] + 1;
        f[k] = x - i0[k];
    }
    auto at = [&](int r, int g, int b, int c) { return t.rgb[((static_cast<size_t>(b) * n + g) * n + r) * 3 + c] / 255.0; };
    for (int c = 0; c < 3; c++) {
        double v = 0;
        for (int corner = 0; corner < 8; corner++) {
            const int r = corner & 1 ? i1[0] : i0[0], g = corner & 2 ? i1[1] : i0[1], b = corner & 4 ? i1[2] : i0[2];
            const double w = (corner & 1 ? f[0] : 1 - f[0]) * (corner & 2 ? f[1] : 1 - f[1]) * (corner & 4 ? f[2] : 1 - f[2]);
            v += w * at(r, g, b, c);
        }
        out[c] = v;
    }
}

}  // namespace

void gradeColor(const GradeParams& p, const GradeLut* artist, const double in[3], double out[3]) {
    double c[3] = {in[0], in[1], in[2]};
    if (artist && artist->size >= 2) sampleLut(*artist, in, c);
    // Exposure and white balance in (approximately) linear light.
    const double ev = std::pow(2.0, p.exposure);
    const double wb[3] = {1 + 0.2 * p.temperature, 1 - 0.2 * p.tint, 1 - 0.2 * p.temperature};
    for (int k = 0; k < 3; k++) c[k] = std::pow(std::pow(std::max(c[k], 0.0), 2.2) * ev * std::max(wb[k], 0.0), 1 / 2.2);
    // Contrast around mid grey, then saturation around the pixel's luma.
    for (double& x : c) x = (x - 0.5) * p.contrast + 0.5;
    const double l = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
    for (double& x : c) x = l + (x - l) * p.saturation;
    // Lift / gamma / gain per channel: lift raises the blacks, gain scales the whites, gamma bends the mids.
    for (int k = 0; k < 3; k++) {
        double x = std::clamp(c[k], 0.0, 1.0);
        x = p.gain[k] * (x + p.lift[k] * (1 - x));
        x = std::pow(std::max(x, 0.0), 1 / std::max(p.gamma[k], 0.01));
        out[k] = std::clamp(x, 0.0, 1.0);
    }
}

GradeLut bakeGradeLut(const GradeParams& p, const GradeLut* artist, int n) {
    n = std::clamp(n, 2, 64);
    GradeLut t;
    t.size = n;
    t.rgb.resize(static_cast<size_t>(n) * n * n * 3);
    for (int b = 0; b < n; b++)
        for (int g = 0; g < n; g++)
            for (int r = 0; r < n; r++) {
                const double in[3] = {r / double(n - 1), g / double(n - 1), b / double(n - 1)};
                double o[3];
                gradeColor(p, artist, in, o);
                uint8_t* d = &t.rgb[((static_cast<size_t>(b) * n + g) * n + r) * 3];
                for (int k = 0; k < 3; k++) d[k] = static_cast<uint8_t>(std::lround(o[k] * 255));
            }
    return t;
}

}  // namespace light
