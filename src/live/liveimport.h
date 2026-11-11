#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "document.h"

// Dumps the loaded map's placed static models and their materials straight out of the running
// game (iw4mp.exe or iw4sp.exe) and imports them into a Document as reference geometry, the same target the
// .ff importer fills. The editor reads the game's memory with ReadProcessMemory (see
// live/process.h) and walks GfxWorld -> smodelDrawInsts -> XModel -> XSurface, transforming each
// model instance by its world placement. Materials' colorMap images are collected so the models
// show textured. Nothing is written to the game.
//
// Requires a map to be loaded in the running game (in a match or via devmap). The models come in
// as a read-only RefMesh; they are not editable brushes.
namespace live {

struct LiveStats {
    std::string game;                    // the exe dumped from: "iw4mp.exe" or "iw4sp.exe"
    std::string mapName;                 // the GfxWorld name, e.g. "maps/mp/mp_highrise.d3dbsp"
    size_t models = 0;                   // placed static-model instances imported
    size_t uniqueModels = 0;             // distinct XModels among them
    size_t modelsSkipped = 0;            // instances dropped (null model, unreadable surfaces)
    size_t surfaces = 0, triangles = 0;  // geometry totals
    size_t materials = 0, images = 0;    // distinct materials, and how many resolved a colorMap
    std::string notes;
};

// exeName: "iw4mp.exe" or "iw4sp.exe", or nullptr to use whichever is running (MP first).
// Imports from the first running process named exeName. Returns false with a human-readable
// error (game not running, no map loaded, access denied, ...).
bool importFromProcess(Document& doc, LiveStats& stats, std::string& error, const char* exeName = nullptr);

// --- Live model library: XModels read by name from the running game, for drawing entities -----
// Geometry is in the model's own space (top LOD); the editor transforms it by each entity's
// origin/angles/modelscale, so models follow their entities while editing.
struct LiveModelSurf {
    std::string material, image;     // editor material name and its colorMap image
    std::vector<float> pos, nrm, uv;  // xyz, xyz, uv per vertex
    std::vector<uint16_t> idx;       // triangle list into this surface's vertices
};
struct LiveModel {
    std::string name;
    std::vector<LiveModelSurf> surfs;
    AABB bounds;  // model space
};
using LiveModelLibrary = std::map<std::string, std::shared_ptr<const LiveModel>>;  // key: lower-case name

struct LiveModelStats {
    std::string game;  // the exe read from
    size_t requested = 0, found = 0, surfaces = 0, triangles = 0;
    std::vector<std::string> missing;  // requested names the running game hasn't loaded
};

// Lower-cases a model name the way the library keys it.
std::string modelKey(std::string name);

// Looks up each name in the running game's XModel pool and adds the ones it has to `lib`
// (replacing older copies). A model is only available if the game currently has it loaded (the
// current map's zones plus the common ones).
bool loadLiveModels(const std::vector<std::string>& names, LiveModelLibrary& lib, LiveModelStats& stats,
                    std::string& error, const char* exeName = nullptr);

// Diagnostic: prints (to the returned string) the module base, the asset-type-name table, and the
// GfxWorld pool pointer/count, so we can tell whether the base+RVA addressing lands on the real
// globals. Used by `iw4r_zone liveprobe`.
bool probeProcess(std::string& report, std::string& error, const char* exeName = nullptr);

// Diagnostic: the loaded map's render setup as the game holds it - surfaces with their material,
// technique set and lighting flags, lightmap images, sun primary light, light grid. Used by
// `iw4r_zone liveworld` to check what a compiled map turned into in-game.
bool inspectWorld(std::string& report, std::string& error, const char* exeName = nullptr);

}  // namespace live
