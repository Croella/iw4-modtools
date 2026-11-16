#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "document.h"
#include "light/colorgrade.h"
#include "light/gradevolume.h"

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
    bool gpu = true;        // trace on the GPU (D3D11 compute) when there is one; else / on failure the CPU
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
// fog setting is written, defaults included. player clip faces are written as "occluder" triangles
// (sun blockers for the shafts). Always written (enabled 0 when off, or no sun and no
// fog) so a stale file never keeps the effect on.
// Ambient occlusion (SSAO, from the depth buffer): volumetric_ssao (1 = on), volumetric_ssao_radius
// (world units), _ssao_intensity, _ssao_bias (0..1: ignores flatter creases), _ssao_near (no AO
// closer than this: keeps it off the first-person weapon), _ssao_distance (fades out by here).
// Contact shadows (short screen-space sun shadows: feet on the floor, props on the ground, which
// the engine's shadow maps miss): volumetric_contact (1 = on), _contact_intensity (0..1, 0.6),
// _contact_length (how far each pixel looks towards the sun, units, 32), _contact_thickness
// (how deep an occluder is assumed to be, 24), _contact_distance (fades out by here, 2500),
// _contact_near (nothing closer: the viewmodel, 20). Needs the sun.
// Colour grading: volumetric_grade (1 = on), volumetric_grade_exposure (stops), _grade_contrast,
// _grade_saturation, _grade_temperature (-1 cool .. 1 warm), _grade_tint (-1 green .. 1 magenta),
// _grade_lift / _grade_gamma / _grade_gain "r g b", _grade_vignette (0..1), _grade_lut <name>
// (zone_source/luts/<name>.png strip LUT, applied before the other settings). All baked into one
// 32^3 table ("lut"/"lutrow" lines in the .volumetric). debug 4 = AO only.
// Per-area grading: "trigger_grade" brush entities with grade_* keys (same names without the
// volumetric_ prefix; unset ones inherit worldspawn's), blend (units inside the edge to fade in,
// default 64), blend_time (seconds to ease in/out, default 0) and priority (higher applies later,
// on top). Each gets its own baked table; the renderers blend them by the camera position.
// Depth of field: volumetric_dof (1 = on), _dof_near_start / _dof_near_end (full near blur at and
// before near_start, sharp from near_end), _dof_far_start / _dof_far_end (sharp to far_start, full
// blur from far_end), _dof_near_blur / _dof_far_blur (max blur radius, pixels at 1080p),
// _dof_weapon (closer than this = the first-person weapon, kept sharp), _dof_auto (1 = focus on
// what's under the crosshair: the four distances become offsets from the focus point, near ones
// towards the camera), _dof_auto_speed (how fast the focus follows, 1/s). debug 5 = blur amount.
// Sun flare: volumetric_flare (1 = on), _flare_intensity, _flare_halo (core, halo, starburst),
// _flare_ghosts (lens ghosts + halo ring), _flare_streak (anamorphic streak), _flare_color "r g b"
// (default the volumetric colour). Fades with how much sky is visible around the sun.
// Glare: volumetric_glare (1 = on), _glare_threshold (0..1 brightness that starts glowing),
// _glare_intensity, _glare_radius (pixels at 1080p, max 64), _glare_sun (veiling glare looking
// towards the sun).
// Screen-space weather: volumetric_weather (0 off, 1 rain, 2 snow), _weather_intensity, _weather_speed
// (fall speed multiplier), _weather_wind "x y" (world units/s), _weather_distance (farthest layer),
// _weather_color "r g b", _weather_opacity. "trigger_weather" brush entities shelter the camera:
// weather_intensity (multiplier inside, default 0), blend (units, 64), blend_time (seconds, 0.5).
// Local light shafts: volumetric_lights (1 = on) scatters every "light" entity (point and spot)
// into the air, with occluder streaks; _lights_intensity, _lights_shadows (0..1 streak strength),
// _lights_max (lights drawn per frame, nearest/brightest first, max 16), _lights_distance (lights
// farther from the camera than this are skipped). Per light keys: see PointLight.
struct GradeVolume {
    std::string name;  // targetname, or "trigger_grade #<entity>"
    std::vector<gradevol::Brush> brushes;
    double blend = 64, blendTime = 0, vignette = 0;
    int priority = 0;
    GradeParams params;
    std::shared_ptr<const GradeLut> lut;
};
struct VolLight {
    Vec3 origin, color;  // colour * intensity * volumetric_intensity
    double radius = 300;
    bool spot = false;
    Vec3 spotDir{0, 0, -1};
    double cosOuter = -1, cosInner = -1, flicker = 0;
};
struct WeatherVolume {
    std::string name;
    std::vector<gradevol::Brush> brushes;
    double blend = 64, blendTime = 0.5, intensity = 0;
};
struct Volumetric {
    bool enabled = false, hasSun = true, shafts = true, fog = false;
    bool ssao = false, grade = false;
    double ssaoRadius = 64, ssaoIntensity = 1.5, ssaoBias = 0.1, ssaoNear = 20, ssaoDistance = 3000;
    bool contact = false;
    double contactIntensity = 0.6, contactLength = 32, contactThickness = 24, contactDistance = 2500, contactNear = 20;
    GradeParams gradeParams;
    double vignette = 0;
    std::string gradeLutName;                  // worldspawn value
    std::shared_ptr<const GradeLut> gradeLut;  // baked table (null when grading is off)
    std::string gradeWarning;                  // e.g. the LUT image couldn't be read
    std::vector<GradeVolume> gradeVolumes;     // sorted by priority
    bool dof = false, dofAuto = false;
    double dofNearStart = 0, dofNearEnd = 0, dofFarStart = 1000, dofFarEnd = 4000, dofNearBlur = 6, dofFarBlur = 4, dofWeapon = 24,
           dofAutoSpeed = 4;
    bool flare = false, glare = false;
    double flareIntensity = 1, flareHalo = 1, flareGhosts = 1, flareStreak = 0.5;
    Vec3 flareColor{1, 0.95, 0.85};
    double glareThreshold = 0.8, glareIntensity = 0.6, glareRadius = 24, glareSun = 0.3;
    int weather = 0;  // 0 off, 1 rain, 2 snow
    double weatherIntensity = 1, weatherSpeed = 1, weatherDistance = 1200, weatherOpacity = 0.35;
    double weatherWind[2] = {0, 0};
    Vec3 weatherColor{0.7, 0.74, 0.8};
    std::vector<WeatherVolume> weatherVolumes;
    bool lights = false;
    double lightsIntensity = 1, lightsShadows = 0.8, lightsDistance = 4000;
    int lightsMax = 8;
    std::vector<VolLight> volLights;
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
// accepted as the radius, Quake style). Spotlights: fov_outer (cone half-angle in degrees) and
// fov_inner (full brightness inside; default 80% of the outer), or just a target; aimed at the
// entity named by target, else along angles / angle. Volumetric (local light shafts, see
// Volumetric): volumetric "0" leaves a light out, volumetric_intensity scales its shaft,
// volumetric_flicker (0..1) makes it flicker, volumetric_only "1" = shaft only (not baked).
struct PointLight {
    Vec3 origin, color;  // color already multiplied by intensity
    double radius = 300;
    bool spot = false;
    Vec3 spotDir{0, 0, -1};
    double cosOuter = -1, cosInner = -1;  // spot cone (cosines of the half-angles)
    bool bake = true, shafts = true;
    double shaftScale = 1, flicker = 0;
};
std::vector<PointLight> readLights(const Document& doc);

bool isSkyMaterial(const std::string& name);
// Player clip contents (0x10000: full_clip, clip_player, clip_ai, ...): invisible, but stops the
// volumetric sun (shafts) in the game and the editor.
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
    std::string device;      // what traced the rays: the GPU adapter's name, or "CPU (n threads)"
    std::string gpuNote;     // why the GPU wasn't used, when it was asked for and the CPU did the work
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
