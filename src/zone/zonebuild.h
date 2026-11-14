#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <memory>
#include "clipmap.h"
#include "gfxworld.h"
#include "document.h"
#include "zonewriter.h"
#include "light/lightbake.h"
#include "zone/skysource.h"
#include "zone/matdefs.h"
#include "zone/modelbuild.h"
#include "zone/weapondef.h"

// Builds an unsigned IW4 (x64) fastfile zone from map data. Scope so far: the three simple world
// assets (ComWorld, GameWorldMp, FxWorld) and collision (clipMap_t, carrying the entity lump as
// an inline MapEnts). Rendering (GfxWorld) comes next.
//
// The zone stream follows the loader model reversed from iw4mp_2013.exe (see zonewriter.h):
//   - Layout: XFile(40) | XAssetList(32) | script-string ptrs + strings | asset array | assets
//   - XFile.size = inflated size - 40; blockSize[] computed by ZoneWriter, exact per loader
//   - XAssetList.assets = -1; .strings = -1 when there are script strings, else 0 (count 0)
//   - each XAsset.header = -1 (inline) or -2 (inline + insert a referencable pointer slot)
//   - a pointer inside asset data: -1 = data follows inline in load order, offsetPointer() = back-ref
//   - the file stream is packed; alignment only moves the in-memory block positions
// Validated byte-for-byte against the stock patch_code_pre_gfx.ff (see tests).

enum IW4AssetType : uint32_t {
    IW4_CLIPMAP_SP = 0x0E,
    IW4_CLIPMAP_MP = 0x0F,
    IW4_COMWORLD = 0x10,
    IW4_GAMEWORLD_SP = 0x11,
    IW4_GAMEWORLD_MP = 0x12,
    IW4_MAPENTS = 0x13,
    IW4_FXWORLD = 0x14,
    IW4_GFXWORLD = 0x15,
    IW4_RAWFILE = 0x24,
    IW4_ADDON_MAP_ENTS = 0x2A,
    IW4_XMODEL = 0x04,
    IW4_XANIMPARTS = 0x02,
    IW4_WEAPON = 0x1C,
};

struct ZoneAsset {
    IW4AssetType type;
    // Emits the asset exactly as its Load_<Asset> walks it: the top-level struct (the writer is
    // already in TEMP and aligned), then sub-data under push(VIRTUAL)/pop.
    std::function<void(zone::ZoneWriter&)> load;
    uint64_t header = zone::kPtrInsert;  // XAsset.header sentinel: kPtrInsert or kPtrFollow
};

// The entity lump text MapEnts.entityString holds: every entity as a { "key" "value" ... } block.
// Brushes are geometry and are not part of the entity string.
std::string documentToEntityString(const Document& doc);

// Individual asset builders (exposed for testing).
ZoneAsset buildMapEnts(const std::string& name, const std::string& entityString);
// withSun: primary lights {0: empty, 1: sun}, matching GfxWorld's primaryLightCount 2.
// env: the baked map's sun (direction + linear colour) for primary light 1; nullptr = default.
ZoneAsset buildComWorld(const std::string& name, bool withSun = false, const light::Environment* env = nullptr);
ZoneAsset buildGameWorldMp(const std::string& name);
// Singleplayer game world: no pathnodes, vehicle tracks or glass (every pointer null).
ZoneAsset buildGameWorldSp(const std::string& name);
ZoneAsset buildFxWorld(const std::string& name);

// State shared between world assets written into the same zone (in load order).
struct SharedWorld {
    bool havePlanes = false;
    uint32_t planesAt = 0;  // VIRTUAL position of GfxWorld's plane array
};
// Rendering. Writes the plane array and records it in `shared` for clipMap to back-reference.
// skySrc: the embedded sky's stock pieces (needed when gw has sky surfaces), or nullptr.
ZoneAsset buildGfxWorld(const std::string& name, const std::string& baseName, const gfx::GfxWorldData& gw,
                        const clip::ClipMapData& cm, std::shared_ptr<SharedWorld> shared,
                        std::shared_ptr<const sky::Source> skySrc = nullptr,
                        std::shared_ptr<const matdef::Library> materials = nullptr);
// Collision. The MapEnts (entity lump) is loaded inline through clipMap_t.mapEnts, as in stock.
// With a GfxWorld earlier in the zone (shared->havePlanes), planes back-reference its array.
// withDefaultStage: give the MapEnts one default stage (required by SP, see writeMapEnts).
ZoneAsset buildClipMap(const std::string& name, const clip::ClipMapData& cm, const std::string& entityString,
                       std::shared_ptr<SharedWorld> shared = nullptr, bool withDefaultStage = false);
// compressedLen 0 = stored uncompressed: buffer holds len bytes + NUL (len+1 total).
ZoneAsset buildRawFile(const std::string& name, int compressedLen, int len,
                       std::vector<uint8_t> buffer);
// A zlib-compressed rawfile, the form stock zones use for any rawfile with content.
ZoneAsset buildCompressedRawFile(const std::string& name, const std::string& text);
// Which game a map zone is built for. MP: maps/mp/<map>.d3dbsp, clipmap_mp + game_map_mp, the
// MP _load script. SP: maps/<map>.d3dbsp, clipmap_sp + game_map_sp, the SP _load script.
enum class MapTarget { MP, SP };

// The map script the game runs at map start (maps/mp/<map>.gsc or maps/<map>.gsc main()).
std::string mapScriptSource(const std::string& mapName, MapTarget target = MapTarget::MP);
// "maps/mp/" or "maps/": where the target's bsp and map script live.
std::string mapAssetDir(MapTarget target);

// Assembles the inflated zone image from assets (in load order). scriptStrings excludes the
// implicit null entry 0; when non-empty the pool is written as [null, strings...].
std::vector<uint8_t> buildZoneImage(const std::vector<ZoneAsset>& assets,
                                    const std::vector<std::string>& scriptStrings = {});

// Full pipeline: map -> the standard minimal asset set -> inflated zone -> IWffu100 fastfile.
// mapName is the zone/base name (e.g. "mp_test"). Returns false with an error on failure.
struct CompileOptions {
    bool bakeLighting = true;               // false: uniform placeholder lighting (fast)
    light::Settings light;                  // bake quality
    const light::Bake* prebaked = nullptr;  // an up-to-date bake of this doc (skips baking)
    light::Progress* progress = nullptr;
    // Sky: the stock sky shader set is copied from <gameDir>/zone/english/trainer.ff once and
    // cached in cacheDir. If that fails the sky is left out and a warning is added.
    std::string gameDir = "C:/Program Files (x86)/Steam/steamapps/common/Call of Duty Modern Warfare 2";
    std::string cacheDir;
    std::vector<std::string>* warnings = nullptr;
    // Stock world-material definitions to embed (see matdef). Null: loaded from
    // <cacheDir>/materialdefs.bin if there is one; materials not in it stay by-name references.
    std::shared_ptr<const matdef::Library> materials;
    // Weapon pack (SP): a stock Special Ops add-on zone whose weapons, models, anims and sounds
    // load with the map. The game loads add-on zones for levels named so_<mission>_<base>
    // (world = <base>.ff, add-on = so_<mission>_<base>.ff; Com_GetBspFilename / iw4sp+0x25e650),
    // so with addonLevel set (weaponPackLevel(mapName)), the zone also gets that level's script
    // maps/<addonLevel>.gsc and an empty add-on entity lump maps/<addonLevel>.mapents; the stock
    // pack is then deployed as <addonLevel>.ff (deployWeaponPack) and played with devmap <addonLevel>.
    std::string addonLevel;
    // Custom content: <map folder>/zone_source. zone_source/xmodels/*.xmodel_export compile into
    // XModels (asset name = file stem), textures come from zone_source/images (png/tga, embedded)
    // or, failing that, stream from the game's IWDs by name. Place them with script_model
    // entities ("model" key); misc_model entities using them become script_models.
    std::string zoneSourceDir;
    // The map's own script (maps/[mp/]<map>.gsc source). Empty: the generated minimal one
    // (mapScriptSource). Editor and CLI read it from <map>.gsc next to the .map (mapScriptPath).
    std::string mapScript;
};

// Weapon packs: worldspawn key "weaponpack" names a stock SP Special Ops add-on zone.
struct WeaponPack {
    const char* zone;     // stock add-on zone (zone/english/<zone>.ff)
    const char* weapons;  // what it carries (from a scan of the zone; informational)
};
const std::vector<WeaponPack>& weaponPacks();
// The level name that loads <mapName>.ff plus the pack: "so_w_<mapName>".
std::string weaponPackLevel(const std::string& mapName);
// Copies <gameDir>/zone/english/<pack>.ff to <deployDir>/<weaponPackLevel(mapName)>.ff.
bool deployWeaponPack(const std::string& gameDir, const std::string& deployDir, const std::string& pack,
                      const std::string& mapName, std::string& error);

// zone_source next to a .map.
std::string zoneSourcePath(const std::string& mapPath);
// Loads every zone_source/xmodels/*.xmodel_export (warnings for skipped ones).
// hints: per-model build options by asset name (from loadZoneSourceWeapons).
std::vector<modelbuild::Model> loadZoneSourceModels(const std::string& zoneSourceDir, std::vector<std::string>& warnings,
                                                    const std::map<std::string, modelbuild::BuildOptions>* hints = nullptr);

// zone_source/weapons/<name> (IW4 weapon files), with model/anim references resolved to
// zone_source assets, and the build options their models need (hidden tags, zero trans).
struct ZoneSourceWeapons {
    std::vector<weapondef::Weapon> weapons;
    std::map<std::string, modelbuild::BuildOptions> hints;
};
ZoneSourceWeapons loadZoneSourceWeapons(const std::string& zoneSourceDir, std::vector<std::string>& warnings);

// The custom map script that goes with a .map: same folder and name, ".gsc" extension.
std::string mapScriptPath(const std::string& mapPath);
// Reads mapScriptPath(mapPath) into out; false (out empty) if there is no such file.
bool readMapScript(const std::string& mapPath, std::string& out);
bool compileMapToFastfile(const Document& doc, const std::string& mapName,
                          std::vector<uint8_t>& fastfileOut, std::string& error,
                          MapTarget target = MapTarget::MP, const CompileOptions& options = {});
