#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

// Effects: IW EffectsEd text files ("iwfx 1|2|3", the format the CoD4/WaW/BO1 mod tools write; BO1
// ships ~4000 under raw/fx) compiled into IW4 x64 FxEffectDef data.
//
// Conversion (verified against stock: BO1's raw/fx/muzzleflashes/saw_flshview_c.efx is the source
// of MW2 common.ff's compiled muzzleflashes/saw_flshview_c, every field matches):
//   graphs are "key scale { {t v ...} {t v ...} }": two piecewise-linear curves; the second only
//     counts when the matching editor flag (useRandVel0, useRandSize0, ...) is on, as the
//     amplitude (base = curve 0, amplitude = curve 1 - curve 0; colours: the second colour)
//   velocity (units/s = scale * curve) -> velSamples: v / (1000 * intervals), and totalDelta, the
//     running integral (sum of (v[i] + v[i+1]) / 2); the game moves an element by
//     lifeMsec * (totalDelta[i] + v[i] (f - f^2/2) + v[i+1] f^2/2) (iw4mp 0x15c410 / 0x160940)
//   rotation (deg/s) -> rotationDelta rad / (1000 * intervals), rotationTotal its integral
//   size -> half the graph value; colour -> B,G,R,A bytes (alpha from alphaGraph, rgb * alpha
//     with modColorByAlpha); spawnAngles / initialRot degrees -> radians; angleVel deg/s ->
//     rad/ms; gravity / 100; lightingFrac * 255
//   samples are uniform in normalised life, as many intervals as the graphs need
//   elements: looping (editorFlags looping) first, then one-shot
//   totalSize = 40 + 288 * elems + name + samples + visual arrays (exact for stock effects)
namespace efx {

struct Curve {
    std::vector<float> t;                // key times, ascending, 0..1
    std::vector<std::vector<float>> v;   // values per key (1, or 3 for colour)
    std::vector<float> eval(float time) const;
};
struct Graph {
    bool present = false;
    float scale = 1;
    Curve curve[2];
};

struct Element {
    std::string name;
    std::set<std::string> editorFlags, flags;
    std::map<std::string, std::vector<std::string>> keys;  // scalar statements: key -> tokens
    Graph vel[2][3], rot, size[2], scale, color, alpha;
    std::string visualType;            // billboardSprite, model, light, ...
    std::vector<std::string> visuals;  // material / model / effect / sound names
    std::vector<float> trailVerts;     // trailDef: x y texCoord per vertex
    std::vector<int> trailInds;
    float num(const std::string& key, int i = 0, float def = 0) const;
    bool has(const std::string& key) const { return keys.count(key) != 0; }
};

struct Effect {
    int version = 0;
    std::vector<Element> elems;
};

bool parse(const std::string& text, Effect& out, std::string& error);

// What the compiler needs to know about a sprite material: whether something will provide it, and
// its texture atlas (rows x columns of frames).
struct MaterialInfo {
    bool known = false;       // false: nothing provides it; the element is left out
    int rows = 1, cols = 1;
    std::string replacement;  // use this material instead (e.g. a BO1 distortion -> a stock one)
};
using MaterialLookup = std::function<MaterialInfo(const std::string&)>;

// One compiled element, ready for the zone writer (pointer fields are filled in by it).
struct Visual {
    enum Kind { Material, Model, Name, None } kind = None;  // Name: sound alias or effect (runner)
    std::string name;
};
struct ElemOut {
    std::array<uint8_t, 288> def{};       // FxElemDef with every pointer field zero
    std::vector<uint8_t> velSamples;      // 96 each
    std::vector<uint8_t> visSamples;      // 48 each
    uint8_t type = 0;
    std::vector<Visual> visuals;          // decals: 2 per mark (visualCount = visuals / 2)
    std::string onImpact, onDeath, emitted;
    std::vector<uint8_t> trail;           // FxTrailDef (0x30) with zero pointers, or empty
    std::vector<uint8_t> trailVerts;      // 20 each
    std::vector<uint16_t> trailInds;
};
struct Compiled {
    std::string name;
    int32_t flags = 0, totalSize = 0, msecLoopingLife = 0;
    int32_t countLooping = 0, countOneShot = 0, countEmission = 0;
    std::vector<ElemOut> elems;
    std::vector<std::string> dependencies;  // other effects it references (runners, impacts, ...)
};

// name: the asset name ("muzzleflashes/my_flash"). Elements this port can't express (BO1-only
// types, triggered-on-touch elements) are left out with a warning.
bool compile(const Effect& fx, const std::string& name, const MaterialLookup& materials, Compiled& out,
             std::vector<std::string>& warnings, std::string& error);

}  // namespace efx
