#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "zone/weapondef.h"

// The asset manager behind the editor's Assets window: indexes a map's zone_source folder
// (weapons, xmodels, xanims, images, effects, skies, scripts), analyses each asset (model stats and
// textures, anim frames / notetracks / matching model, weapon references), cross-references them
// ("used by", unused assets, problems) and does the file operations (import, rename with reference
// updates, duplicate, new weapon, delete). No UI or D3D here, so the tests drive it directly.
namespace assetman {

enum class Kind { Weapon, XModel, XAnim, Image, Effect, Sky, Script, Other, Count };
constexpr int kKindCount = static_cast<int>(Kind::Count);
const char* kindName(Kind k);    // "Weapons", "XModels", ...
const char* kindSubdir(Kind k);  // "weapons", "xmodels", ... ("" for Other)

struct Asset {
    Kind kind = Kind::Other;
    std::string name;  // asset name as the compiler sees it (lowercase stem; effects/scripts: path under fx/ or scripts/)
    std::filesystem::path path;
    uintmax_t bytes = 0;
    std::vector<std::filesystem::path> lods;  // xmodels: <stem>_lod1.._lod9 files beside it
};

// A weapon file as ordered key/value pairs, so editing keeps the file's order and every key
// (including ones the compiler ignores). Values may contain newlines (hideTags, notetrack maps).
struct WeaponFile {
    std::vector<std::pair<std::string, std::string>> kv;
    bool parse(const std::string& text, std::string& error);
    std::string format() const;  // "WEAPONFILE\key\value..."
    const std::string* get(const std::string& key) const;  // case-insensitive
    void set(const std::string& key, const std::string& value);  // replaces, else appends
    bool erase(const std::string& key);
};

// One reference a weapon makes to another asset.
struct WeaponRef {
    std::string key, value;
    weapondef::RefKind kind = weapondef::RefKind::None;
    std::string resolved;  // the zone_source asset it compiles to ("" = by name: stock, or missing)
};

struct ModelInfo {
    bool ok = false;
    std::string error;
    size_t verts = 0, tris = 0;
    std::vector<std::string> bones;
    struct Mat {
        std::string name, texture;   // as exported
        std::filesystem::path image;  // the file the compiler embeds ("" = not found)
    };
    std::vector<Mat> materials;
    std::vector<std::string> missingTextures;  // used materials whose texture isn't in zone_source
    float mins[3] = {}, maxs[3] = {};
};

struct AnimInfo {
    bool ok = false;
    std::string error;
    int frames = 0;
    float framerate = 30;
    bool looping = false;  // name ends in _loop / _idle (the compiler's looping flag)
    std::vector<std::string> parts;
    std::vector<std::pair<int, std::string>> notes;  // frame, notetrack
    std::string model;  // zone_source model sharing the most bones ("" = none)
    size_t modelBones = 0;
};

struct ImageInfo {
    bool ok = false;
    std::string error;
    int width = 0, height = 0;
    bool alpha = false;  // any pixel with alpha < 255
};

struct WeaponInfo {
    bool ok = false;
    std::string error;
    WeaponFile file;
    std::vector<WeaponRef> refs;
    std::vector<std::string> warnings;  // from the compiler's parser
};

struct Issue {
    enum class Level { Info, Warning, Error } level = Level::Warning;
    Kind kind = Kind::Other;
    std::string asset;  // asset name ("" = general)
    std::string text;
};

struct Index {
    std::string root;  // the zone_source folder
    std::vector<Asset> assets;
    void scan(const std::string& zoneSourceDir);
    const Asset* find(Kind k, const std::string& name) const;
    std::vector<std::string> names(Kind k) const;
    size_t count(Kind k) const;
};

// Everything that takes file parsing; built off the UI thread (analyse) and swapped in.
struct Analysis {
    std::map<std::string, ModelInfo> models;
    std::map<std::string, AnimInfo> anims;
    std::map<std::string, ImageInfo> images;  // images/ and textures next to the models, by file name (lowercase)
    std::map<std::string, WeaponInfo> weapons;
    // (kind, name) -> who uses it: "weapon ak74uwu (gunModel)", "model t5_weapon_ak74u (material m_ak)".
    std::map<std::pair<Kind, std::string>, std::vector<std::string>> usedBy;
    std::vector<Issue> issues;
    size_t issuesFor(Kind k, const std::string& name, Issue::Level atLeast = Issue::Level::Warning) const;
};
Analysis analyse(const Index& index);
// The texture file the compiler embeds for a model material (zone_source/images, then beside the
// models in xmodels; the exported file name, <stem>.png/.tga, and the stem without t5_/t6_/t7_).
std::filesystem::path findTexture(const std::string& zoneSourceDir, const std::string& texture, const std::string& material);

// ---- file operations (all return false + error on failure; nothing is overwritten) ----
// The kind a file would be imported as, from its extension ("" / no extension = weapon file).
Kind importKind(const std::filesystem::path& file);
// Copies file into zone_source/<subdir>; xmodel imports also bring their _lodN siblings and the
// textures their materials name when those sit next to the source file. copied: destination paths.
bool importFile(const std::string& zoneSourceDir, const std::filesystem::path& file, std::vector<std::filesystem::path>& copied,
                std::string& error);
// Renames an asset's file(s) (an xmodel's LOD files too) and, with updateRefs, rewrites weapon
// files whose references resolve to it. changed: weapon files rewritten.
bool renameAsset(const Index& index, const Asset& a, const std::string& newName, bool updateRefs,
                 std::vector<std::filesystem::path>& changed, std::string& error);
bool duplicateWeapon(const Index& index, const Asset& a, const std::string& newName, std::string& error);
// A new weapon file with the keys a working rifle needs, pointing at the given models (may be "").
bool newWeapon(const std::string& zoneSourceDir, const std::string& name, const std::string& gunModel,
               const std::string& worldModel, std::string& error);
bool saveWeapon(const Asset& a, const WeaponFile& w, std::string& error);
bool validName(const std::string& name);  // [a-z0-9_] only, not empty

}  // namespace assetman
