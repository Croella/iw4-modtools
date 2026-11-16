#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Colour grading for the in-game post pass (iw4r_mapload.dll) and the editor preview: the
// worldspawn grade settings (and an optional artist LUT) are baked into one 3D lookup table, so
// both renderers only do a single texture lookup per pixel.
namespace light {

// RGB8 3D table: entry (r, g, b) at rgb[((b * size + g) * size + r) * 3].
struct GradeLut {
    int size = 0;
    std::vector<uint8_t> rgb;
};

struct GradeParams {
    double exposure = 0;     // stops (+1 = twice as bright, in linear light)
    double contrast = 1;     // around mid grey; 1 = unchanged
    double saturation = 1;   // 0 = grey, 1 = unchanged
    double temperature = 0;  // -1 cool .. +1 warm
    double tint = 0;         // -1 green .. +1 magenta
    double lift[3] = {0, 0, 0}, gamma[3] = {1, 1, 1}, gain[3] = {1, 1, 1};
};

// Reads an artist LUT image: the usual horizontal strip (width = size * size, height = size; red
// across each square, green down, blue across the squares - what "neutral LUT" PNGs for
// Photoshop/Resolve/Unreal look like), size 2..64.
bool loadStripLut(const std::string& path, GradeLut& out, std::string& error);
// A neutral (identity) strip LUT image, to paste into a screenshot and grade in an image editor.
bool saveNeutralStripLut(const std::string& path, int size, std::string& error);

// One colour (display values 0..1) through the artist LUT (if any) then the parameters.
void gradeColor(const GradeParams& p, const GradeLut* artist, const double in[3], double out[3]);
// The table both renderers sample.
GradeLut bakeGradeLut(const GradeParams& p, const GradeLut* artist, int size = 32);

}  // namespace light
