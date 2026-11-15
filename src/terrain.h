#pragma once
#include <string>
#include <vector>

#include "brush.h"
#include "document.h"

// Sculptable terrain made of brushes. An "iw4r_terrain" point entity holds a height field (corner
// heights over a grid of square cells); every cell is two convex triangular-prism brushes in the
// world (sloped top, flat bottom at the terrain's depth), tagged with the brush attribute
// "iw4r_terrain <id> <i> <j> <t>;". They are ordinary world brushes, so compiling, collision,
// lighting and surface types need nothing special; the entity is left out of the compiled map.
// Sculpting changes the heights and rebuilds only the cells it touched.
namespace terrain {

inline constexpr const char* kClass = "iw4r_terrain";

// Texture painting by slope (and optionally height): the first rule that matches a triangle's
// slope (degrees from flat) and mean height paints its top; none matching leaves Field::material.
// Outer walls are painted as 90-degree slopes. Stored on the entity, so sculpting repaints as it goes.
struct PaintRule {
    std::string material;
    double minSlope = 0, maxSlope = 90;      // degrees
    double minHeight = -1e6, maxHeight = 1e6;  // relative to the terrain's origin
};

struct Field {
    std::string id;          // "terrainid" key; ties the brushes to the entity
    Vec3 origin;             // the grid's min corner; heights are relative to origin.z
    int nx = 16, ny = 16;    // cells
    double cell = 64;        // cell size
    double depth = 64;       // the brushes' bottom: origin.z - depth
    std::string material = "caulk";
    std::vector<PaintRule> paint;
    double paintJitter = 0;  // degrees of per-triangle randomness on the slope thresholds (breaks up straight lines)
    std::vector<double> h;   // (nx + 1) * (ny + 1) corner heights, row-major (j * (nx + 1) + i)
    double& at(int i, int j) { return h[static_cast<size_t>(j) * (nx + 1) + i]; }
    double at(int i, int j) const { return h[static_cast<size_t>(j) * (nx + 1) + i]; }
    double worldX(int i) const { return origin.x + i * cell; }
    double worldY(int j) const { return origin.y + j * cell; }
    // Height (relative) at a world xy, bilinear over the cell; false outside the grid.
    bool heightAt(double x, double y, double& out) const;
};

bool read(const Entity& e, Field& out);
void write(const Field& f, Entity& e);

// The two brushes of cell (i, j) (t = 0, 1), material and tag set.
void cellBrushes(const Field& f, int i, int j, Brush out[2]);
std::string tag(const Field& f, int i, int j, int t);
bool parseTag(const Brush& b, std::string& id, int& i, int& j, int& t);

// The material painted on a triangle of cell (i, j) with the given slope and mean height.
std::string paintMaterial(const Field& f, double slopeDeg, double height, int i, int j, int t);
// True if `m` is a material the terrain itself assigns (its base material or a rule's), i.e. not
// one set on a cell by hand.
bool autoMaterial(const Field& f, const std::string& m);
// Repaints every cell from the rules without rebuilding geometry. Hand-set textures (not automatic
// in `f` or `prev`) are kept unless `force`. Returns the number of faces changed.
int repaint(Document& doc, const Field& f, const Field* prev, bool force);

struct PaintPreset {
    const char* name;
    std::vector<PaintRule> rules;
};
const std::vector<PaintPreset>& paintPresets();

// Creates the entity and all its brushes in the document (worldspawn). Returns the entity index.
int create(Document& doc, Field f);
// Rebuilds the brushes of the cells in [i0, i1] x [j0, j1] (corner-index range is clamped). Brushes
// missing from the world (e.g. deleted by hand) are added back.
void rebuildCells(Document& doc, const Field& f, int i0, int j0, int i1, int j1);
// Removes the entity's brushes (before deleting a terrain or regenerating it with a new size).
void removeBrushes(Document& doc, const std::string& id);

enum class Tool { Raise, Lower, Smooth, Flatten, Noise, Melt, Paint };
inline constexpr Tool kTools[] = {Tool::Raise, Tool::Lower, Tool::Smooth, Tool::Flatten, Tool::Noise, Tool::Melt, Tool::Paint};
const char* toolName(Tool t);
const char* toolTip(Tool t);

struct Stroke {
    Tool tool = Tool::Raise;
    double radius = 256, strength = 64;  // strength: units per second at the centre (Raise/Lower/Noise)
    double flattenTo = 0;                // Flatten: the height (relative) under the stroke's first point
    unsigned seed = 1;
    // Paint: the material, how ragged the brush's rim is (0 = a hard circle, 1 = scattered over the
    // whole radius) and the slopes it may paint (degrees).
    std::string material;
    double scatter = 0.4, minSlope = 0, maxSlope = 90;
};
// One application of a tool at world (x, y) for `dt` seconds. Returns false if it touched nothing;
// else the changed corner range.
bool apply(Field& f, const Stroke& s, double x, double y, double dt, int& i0, int& j0, int& i1, int& j1);

// Paint brush: sets the top texture of the triangles under a circle at (x, y) and marks them
// hand-painted ("iw4r_painted;" brush attribute), so slope painting and sculpting leave them alone.
// `erase` instead clears the mark and gives them back their slope-rule texture. Returns the number
// of triangles changed.
inline constexpr const char* kPaintedAttr = "iw4r_painted;";
bool painted(const Brush& b);
int paint(Document& doc, const Field& f, const Stroke& s, double x, double y, bool erase);

// Fractal hills/mountains over the whole field (fBm value noise), 0..height, with `octaves` layers.
void mountains(Field& f, double height, int octaves, unsigned seed);

}  // namespace terrain
