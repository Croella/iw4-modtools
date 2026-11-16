#pragma once
#include <array>
#include <set>
#include <cstdint>
#include <string>
#include <vector>
#include "clipmap.h"
#include "document.h"

namespace light {
struct Bake;
}

// Render-world compiler: editor brush faces -> the arrays GfxWorld holds. Serialization is in
// zonebuild.cpp; the loader traversal it follows is Load_GfxWorld (0x11ae60).
//
// Conventions taken from stock boneyard's GfxWorld:
//   - GfxWorldVertex (44 bytes): xyz, binormalSign (+-1), color, texCoord, lmapCoord, normal,
//     tangent; normal/tangent are PackedUnitVec: byte = v*127 + 127, 4th byte 63 (scale 1/127)
//   - surfaces are grouped by material; indices are relative to the surface's firstVertex and
//     baseIndex advances by triCount*3; laf = {lightmapIndex, reflectionProbeIndex,
//     primaryLightIndex, flags=1} (lightmap 31 = none)
//   - dpvs node stream (u16): v <= cellCount is a leaf for cell v-1 (0 = solid); larger values
//     are interior nodes on plane v-cellCount-1, followed by the back-child offset. A one-cell
//     world is nodes = {1}
//   - sortedSurfIndex holds staticSurfaceCount entries, then the same again for "no decal";
//     aabb trees index into it (no-decal ranges start at staticSurfaceCount)
//   - dpvs.surfaceMaterials (the 64-bit draw-surface sort keys) are RUNTIME: the game builds them
//   - light grid: 32x32x64 cells, coord = floor((pos + 131072) / size); rows along rowAxis, each a
//     header {colStart, colCount, zStart, zCount, firstEntry} plus lookupTable runs
//     (cols, zCount, zStart) for filled columns or (cols, 0) for empty ones; entries column-major
//   - lightmaps: primary D3DFMT_L8 at 2x the secondary's width, secondary A8R8G8B8
//   - reflection probes: A8R8G8B8 cube images with a full mip chain
namespace gfx {

struct Vertex {
    float xyz[3];
    float binormalSign;
    uint32_t color;
    float texCoord[2];
    float lmapCoord[2];
    uint32_t normal;   // PackedUnitVec
    uint32_t tangent;  // PackedUnitVec
};

struct Surface {
    uint32_t firstVertex = 0;
    uint16_t vertexCount = 0;
    uint16_t triCount = 0;
    uint32_t baseIndex = 0;
    uint32_t material = 0;  // into GfxWorldData::materials
    uint8_t primaryLight = 0;  // laf.primaryLightIndex: 1 = lit by the sun primary light
    uint8_t probe = 0;         // laf.reflectionProbeIndex: the nearest reflection probe
    uint8_t lightmap = 0;      // laf.lightmapIndex (page) for lightmapped surfaces
    bool sky = false;          // drawn with the embedded sky material (no lightmap)
    bool unlit = false;        // unlit effect material (no lightmap, no sun)
    float mins[3], maxs[3];
};

struct Image {
    std::string name;
    uint8_t mapType;   // 3 = 2D, 5 = cube
    uint8_t semantic;
    uint8_t category;
    uint16_t width, height;
    uint8_t levelCount;  // as stored in GfxImageLoadDef (0 for the stock full-mip cubes)
    uint32_t loadFlags;
    uint32_t format;     // D3DFORMAT
    std::vector<uint8_t> pixels;
};

struct LightGridRow {
    uint16_t colStart, colCount, zStart, zCount;
    uint32_t firstEntry;
    std::vector<uint8_t> lookupTable;  // padded to 4 bytes
};

struct LightGrid {
    uint16_t mins[3] = {}, maxs[3] = {};
    uint32_t rowAxis = 1, colAxis = 0;
    std::vector<uint16_t> rowDataStart;  // per row, in 4-byte units into rawRowData
    std::vector<uint8_t> rawRowData;
    uint32_t entryCount = 0;
    uint32_t colorCount = 1;
    uint8_t gray = 128;  // the uniform color every entry uses (when unbaked)
    // Baked grid (empty = uniform gray): per entry {colorsIndex u16, primaryLightIndex u8,
    // needsTrace u8} packed little-endian, and the colour palette: GfxLightGridColors, 56 rgb
    // (light::gridDirections order), each byte = sqrt(light) * 127.5 (the model shaders square
    // twice the texel).
    std::vector<uint32_t> entries;
    std::vector<std::array<uint8_t, 168>> colors;
};

// A primary spotlight (engine primary light 2 + index): ComPrimaryLight values, the light region
// (one hull: a 9-axis k-DOP relative to the origin, axes x y z x+y x-y x+z x-z y+z y-z
// unnormalized - RE'd from stock hulls) and the surfaces that cast its shadows.
struct PrimaryLight {
    float origin[3], dir[3];  // dir: towards the light (minus the spot direction), as stock
    float color[3];
    float radius, cosOuter, cosInner;
    float kdopMid[9], kdopHalf[9];
    std::vector<uint16_t> shadowSurfs;  // sorted surface indices
};

struct GfxWorldData {
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<Surface> surfaces;
    std::vector<std::string> materials;
    float mins[3], maxs[3];
    std::vector<Image> lightmapPrimaries, lightmapSecondaries;  // one pair per lightmap page
    // Reflection probes: baked 64x64 cubes at the map's reflection_probe entities (else one at a
    // spawn point / the middle); unbaked maps get one tiny uniform cube.
    std::vector<Image> reflectionProbes;
    std::vector<std::array<float, 3>> probeOrigins;
    LightGrid lightGrid;
    std::vector<PrimaryLight> primaries;
    // Sky: faces with a sky material become surfaces of one embedded sky material (index
    // skyMaterial into materials, -1 = no sky); skyImage is the cubemap streamed from
    // images/<skyImage>.iwi.
    int skyMaterial = -1;
    std::vector<uint32_t> skySurfaces;
    std::string skyImage;
    // A custom sky from zone_source/skies/<sky>.png|.tga (equirectangular panorama) embedded inline
    // as a cube instead (pixels empty = use the stock image above).
    Image customSky;
    // Surfaces [0, opaqueSurfaceCount) are dpvs "lit opaque", the rest "lit trans" (see
    // sortSurfaces). Default: everything opaque.
    uint32_t opaqueSurfaceCount = UINT32_MAX;
};

// Orders surfaces like stock GfxWorlds: ascending material sort key (sortKeys[material]); keys
// up to kLastOpaqueSortKey are the lit-opaque range, the rest (decals, translucent and additive
// effects) the lit-trans range. Stock trainer: opaque <= 2 (sky last at 2), trans 6..29,
// shadowcaster 34. Remaps skySurfaces.
constexpr uint8_t kLastOpaqueSortKey = 5;
void sortSurfaces(GfxWorldData& gw, const std::vector<uint8_t>& sortKeys);

// True for tool materials that are never drawn (caulk, clip, triggers, hint, sky...).
bool isToolMaterial(const std::string& name);

// bake: baked lighting for the doc (light::bake), or nullptr for the old uniform lighting.
// includeSky: emit sky faces as sky surfaces (needs the embedded sky material at write time).
// unlitMaterials: lower-case editor names drawn without lightmap/sun (additive effects).
bool buildGfxWorldData(const Document& doc, GfxWorldData& out, std::string& error,
                       const light::Bake* bake = nullptr, bool includeSky = false,
                       const std::set<std::string>* unlitMaterials = nullptr);
// The material name GfxWorld uses for the embedded sky.
constexpr const char* kSkyMaterial = "wc/iw4r_sky";

// PackedUnitVec encoding used by world vertices (exposed for tests).
uint32_t packUnitVec(const Vec3& v);
Vec3 unpackUnitVec(uint32_t packed);
// A face's vertex tangent (texture u gradient in the plane) and binormal sign, as the compiler writes them.
Vec3 texGradientOf(const Face& f);
float binormalSignOf(const Face& f);

}  // namespace gfx
