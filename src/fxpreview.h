#pragma once
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "document.h"
#include "zone/efx.h"

// Live effect preview for the editor's 3D view: runs the .efx effects of the map's fx_origin
// entities (zone_source/fx) on the CPU, the way the game plays the compiled FxEffectDef, and hands
// the renderer camera-facing sprites.
//
// Matches the compiler's reading of the .efx (see zone/efx.h): graphs are curve 0, plus a random
// blend towards curve 1 when the element's useRand* editor flag is set; velGraph0 is in the effect's
// axes (X = the fx_origin's forward, Y = left, Z = up), velGraph1 in world axes; size is the sprite's
// width; gravity is a percentage of 800 units/s^2; looping elements spawn one sprite every
// spawnLooping interval (count 0 = forever), one-shot elements spawnOneShot sprites when the effect
// starts; an fx_origin replays effects without looping elements every `delay` seconds (2 by
// default), as the compiled map's script does.
//
// Previewed: sprite elements (billboard, oriented, tail, trail and cloud are all drawn as camera-
// facing sprites), atlases, colour/alpha/size/rotation graphs. Not previewed: models, lights,
// sounds, decals, runners, collision, distance fades.
namespace fxpreview {

struct MaterialInfo {
    bool found = true;  // false: nothing provides it (the element is left out, as the compiler does)
    bool additive = true;
    int rows = 1, cols = 1;
};
using MaterialFn = std::function<MaterialInfo(const std::string&)>;

struct Sprite {
    std::string material;
    bool additive = true;
    Vec3 pos, right, up;  // centre; half-extent axes (already rotated and scaled)
    float uv[4] = {0, 0, 1, 1};  // u0 v0 u1 v1
    float color[4] = {1, 1, 1, 1};
    float depth = 0;  // distance along the view direction (sorting)
};

class Preview {
public:
    // Parses every zone_source/fx/**/*.efx (resets everything). `materials` gives each sprite
    // material's blend mode and atlas.
    void load(const std::string& zoneSourceDir, MaterialFn materials, std::vector<std::string>& warnings);
    bool loaded() const { return loaded_; }
    // Emitters from the document's fx_origin entities. Unchanged ones keep running.
    void sync(const Document& doc);
    void update(double dt);
    // Dynamic effects (see FxPlacement in zone/zonebuild.h): fx_origins with a targetname run as
    // groups that start/stop like the generated script; `viewer` is where the player is (the 3D
    // camera): walking into a trigger_radius that targets a group starts it (stops it, with
    // script_noteworthy "stop"), once per entry.
    void setViewer(const Vec3& viewer);
    void startGroup(const std::string& name);
    void stopGroup(const std::string& name);
    std::set<std::string> groups() const;  // targetnames of the dynamic fx_origins
    size_t runningCount() const;           // emitters currently running (static ones always count)
    // Runs `seconds` of simulation at 30 Hz (so a fresh preview starts populated).
    void warmup(double seconds);
    void sprites(const Vec3& eye, const Vec3& forward, const Vec3& right, const Vec3& up, std::vector<Sprite>& out) const;
    size_t particleCount() const;
    size_t emitterCount() const { return emitters_.size(); }
    size_t effectCount() const { return effects_.size(); }
    std::set<std::string> materials() const;  // every sprite material the loaded effects use
    const std::set<std::string>& missing() const { return missing_; }  // fx names placed but not found

private:
    struct Elem {
        const efx::Element* src = nullptr;
        bool looping = false;
        int interval = 200, loopCount = 0;    // looping
        int oneShotBase = 1, oneShotRand = 0;  // one-shot
        float delayBase = 0, delayRand = 0;   // ms
        float lifeBase = 1000, lifeRand = 0;  // ms
        float org[3][2] = {}, offRadius[2] = {}, offHeight[2] = {};
        int offsetShape = 0;  // 0 none, 1 sphere, 2 cylinder
        float initRot[2] = {}, gravity[2] = {};
        bool randVel0 = false, randVel1 = false, randSize0 = false, randColor = false, randAlpha = false, randRot = false;
        bool modColorByAlpha = false;
        std::vector<std::string> materials;
        std::vector<MaterialInfo> matInfo;
        int atlasBehavior = 0, atlasIndex = 0, atlasRange = 0, atlasFps = 0, atlasEntries = 1;
    };
    struct Effect {
        std::vector<Elem> elems;
        bool hasLooping = false;
        std::unique_ptr<efx::Effect> src;
    };
    struct Particle {
        int elem = 0, material = 0, frame = 0;
        float age = 0, life = 1;  // seconds
        Vec3 pos;
        float gravVel = 0, gravity = 0, rot = 0;
        float r[6] = {};  // random blends: vel0, vel1, size, colour, alpha, rot
    };
    struct ElemState {
        float nextSpawn = 0;  // seconds since the effect started
        int spawned = 0;
    };
    struct Emitter {
        std::string key;  // entity text that defines it (change -> restart)
        std::string effect;
        Vec3 origin, axis[3];  // forward, left, up
        float delay = 0, time = 0, nextReplay = 0;
        std::vector<ElemState> states;
        std::vector<Particle> particles;
        bool started = false;
        // dynamic
        std::string group;
        bool dynamic = false, waitStart = false, once = false, running = true, used = false;
        float duration = 0, stopAt = 0;
    };
    struct Trigger {
        Vec3 origin;
        double radius = 64, height = 128;
        std::string group;
        bool stop = false, inside = false;
        // brush trigger: inside any brush (all of its face planes: normal . p <= dist); empty = radius
        std::vector<std::vector<std::pair<Vec3, double>>> brushes;
    };
    void start(Emitter& em, const Effect& fx, bool loopingToo);
    void run(Emitter& em);  // a dynamic emitter's start
    void spawn(Emitter& em, const Effect& fx, int elemIndex);
    void step(Emitter& em, float dt);

    std::map<std::string, Effect> effects_;
    std::vector<Emitter> emitters_;
    std::vector<Trigger> triggers_;
    std::set<std::string> missing_;
    std::mt19937 rng_{12345};
    bool loaded_ = false;
};

}  // namespace fxpreview
