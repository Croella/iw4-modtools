#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "document.h"

// Static lighting baker shared by the compiler (IW4 lightmaps + light grid) and the editor's
// lighting preview. Traces rays against the worldspawn brushes.
//
// IW4 lights a world surface as (from the stock lit_lmap pixel shaders):
//     albedo^2 * ( (T + B * dirTerm)^2  +  sunMask * sunColor * saturate(N . sunDir) )
// The sun is the real-time sun primary light; its baked part is only the visibility mask
// (lightmap "primary", L8). Everything else - point lights, sky dome, bounce, ambient - is baked
// into the "secondary" lightmap (T/B halves, stored as sqrt of linear light). So this baker keeps
// the two apart: `light` is the non-sun light per luxel, `sunVis` the sun visibility.
namespace light {

struct Settings {
    double luxelSize = 16;  // world units per luxel; the sun mask is baked at twice this density
    int skySamples = 48;    // hemisphere rays per luxel (sky dome + bounce)
    int bounces = 1;        // 0 = direct + sky only
    double albedo = 0.45;   // assumed surface reflectance for bounce light
    int maxAtlas = 1024;    // max luxel atlas width/height (luxelSize grows until the map fits)
    // Unlit see-through materials (additive effects such as atmos_particles_dust), lower-case
    // editor names: they neither block light nor get a lightmap.
    std::set<std::string> unlitMaterials;
};

// Sun, sky and ambient from worldspawn keys (CoD Radiant names; defaults give a daylit map):
//   sundirection "pitch yaw roll" (forward vector points at the sun), suncolor "r g b",
//   sunlight (intensity), diffusefraction (share of the sun's light that comes from the sky dome),
//   sundiffusecolor "r g b" (sky dome tint), ambient (flat fill), _color "r g b" (fill tint).
struct Environment {
    Vec3 sunDir{0, 0, 1};  // unit vector towards the sun
    Vec3 sunColor;         // linear direct sun: what the game's sun primary light adds
    Vec3 skyColor;         // linear irradiance from a fully open sky dome
    Vec3 ambient;          // linear flat fill
    bool hasSun = true;
};
Environment readEnvironment(const Document& doc);

// "<map>.volumetric" settings for iw4r_mapload.dll's sun shafts (see src/dll/volumetric.h), from
// worldspawn: volumetric (1 = on, default; 0 = off), volumetric_intensity, volumetric_density,
// volumetric_decay, volumetric_color "r g b" (default suncolor), volumetric_threshold,
// volumetric_glow, volumetric_glowradius, volumetric_skymode (0 brightness, 1 depth),
// volumetric_skydepth, volumetric_debug (1 mask, 2 depth, 3 fog), volumetric_shafts (0 = fog only).
// Height fog: volumetric_fog (1 = on), volumetric_fog_density, _fog_falloff, _fog_height,
// _fog_color "r g b", _fog_sun, _fog_anisotropy, _fog_start, _fog_max, _fog_sky, _fog_skydist; every
// fog setting is written, defaults included. full_clip faces are written as "occluder" triangles
// (sun blockers for the shafts). Always written (enabled 0 when off, or no sun and no
// fog) so a stale file never keeps the effect on.
struct Volumetric {
    bool enabled = false, hasSun = true, shafts = true, fog = false;
    Vec3 sunDir{0, 0, 1};
    Vec3 color{1, 0.95, 0.85};
    double intensity = 0.7, density = 0.9, decay = 0.97, threshold = 0.25, glow = 1.5, glowRadius = 0.08;
    int skyMode = -1;
    double skyDepth = 30000;
    int debug = 0;
    double fogDensity = 0.0003, fogFalloff = 0.004, fogHeight = 0, fogSun = 0.6, fogG = 0.6, fogStart = 64, fogMax = 0.9,
           fogSky = 1, fogSkyDist = 8000;
    Vec3 fogColor{0.55, 0.6, 0.68};
};
Volumetric readVolumetric(const Document& doc);  // also drives the editor's preview
std::string volumetricConfig(const Document& doc);

// "light" entities: origin, _color, intensity (default 1), radius (default 300; "light" also
// accepted as the radius, Quake style).
struct PointLight {
    Vec3 origin, color;  // color already multiplied by intensity
    double radius = 300;
};
std::vector<PointLight> readLights(const Document& doc);

bool isSkyMaterial(const std::string& name);
// full_clip / clip_full: invisible, but stops the volumetric sun (shafts) in the game and the editor.
bool blocksVolumetricSun(const std::string& name);
// Faces that block light: drawn faces, caulk and sky. Clip/trigger/hint/etc. let light through.
bool blocksLight(const std::string& name);
// True if `name` (any case, with or without "wc/") is in the set.
bool inMaterialSet(const std::set<std::string>& set, const std::string& name);

// Identifies a face by its geometry and material, so a bake can be matched to faces after
// unrelated edits (moving one brush only invalidates that brush's faces).
uint64_t faceKey(const Face& f);
// Hash of everything a bake depends on (worldspawn faces + keys, light entities). A bake whose
// sceneHash matches the document's is up to date (doc.revision also bumps on selection).
uint64_t sceneHash(const Document& doc);

struct FaceMap {
    int x = 0, y = 0, w = 0, h = 0;  // luxel rect in the atlas (1-luxel border included)
    int axisU = 0, axisV = 1;        // world axes the face is projected onto
    double minU = 0, minV = 0;       // world coordinate of the rect's corner
};

class Scene;  // ray-tracing acceleration structure, kept for light-grid sampling

struct Bake {
    int width = 0, height = 0;  // luxel atlas size (powers of two)
    double luxelSize = 16;
    std::vector<float> light;     // linear rgb per luxel: everything except direct sun
    std::vector<uint8_t> sunVis;  // (2*width x 2*height): direct-sun visibility, 0..255
    std::map<uint64_t, FaceMap> faces;
    Environment env;
    uint64_t docRevision = 0;
    uint64_t sceneHash = 0;  // light::sceneHash of the baked document
    std::shared_ptr<const Scene> scene;

    const FaceMap* find(const Face& f) const;
    // Lightmap coordinate (0..1 across the atlas) of point p on a mapped face.
    void lmapCoord(const FaceMap& m, const Vec3& p, float& u, float& v) const;
};

struct Progress {
    std::atomic<float> fraction{0};
    std::atomic<bool> cancel{false};
};

// Bakes the worldspawn's drawn faces. Returns false on cancel or if nothing is drawable.
bool bake(const Document& doc, const Settings& settings, Bake& out, std::string& error,
          Progress* progress = nullptr);

// Light-grid samples (models): non-sun light averaged over all directions, and whether the sun
// reaches the point. Needs a finished bake (for bounce lookups).
struct GridSample {
    Vec3 light;
    bool sun = false;
};
std::vector<GridSample> sampleGrid(const Bake& b, const std::vector<Vec3>& points, int rays = 24);

}  // namespace light
