#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Full definitions of stock world materials, so compiled maps can carry them. A by-name
// material reference (",wc/foo") only resolves if some loaded zone has the material, and world
// materials only ship inside the map zones that use them - so a compiled map that references them
// by name gets the default material (unlit 2d techset, default image) and none of its lighting.
//
// A definition is copied from a stock zone where the material is fully inline (Load_Material
// order: struct 0x78, name, technique set, texture table + images, constants, state bits). The
// technique set is NOT copied: world materials use the lit "wc_l_sm_b0c0[n0][s0]" sets, which
// common.ff (always loaded) has, so the compiled material names one by reference, picked from the
// textures it samples (c0 colorMap, n0 normalMap, s0 specularMap; RE'd from those sets' pixel
// shader samplers). Images keep their stock header and stream from the IWDs by name.
namespace matdef {

struct Texture {
    std::vector<uint8_t> def;          // MaterialTextureDef (16): nameHash, nameStart/End, sampler, semantic
    std::string image;                 // image asset name (images/<name>.iwi)
    std::vector<uint8_t> imageHeader;  // GfxImage (0x28)
    std::vector<uint8_t> loadDef;      // GfxImageLoadDef header (16), resourceSize 0
    char kind() const { return def.size() >= 6 ? def[4] : 0; }  // first letter of the texture name
};

struct Def {
    std::string name;                // full asset name, e.g. "wc/ch_asphalt01"
    std::vector<uint8_t> header;     // Material (0x78)
    std::vector<Texture> textures;
    std::vector<uint8_t> constants;  // 32 per constant
    std::vector<uint8_t> stateBits;  // 8 per entry
};

using Library = std::map<std::string, Def>;  // key: lower-case full name

// Adds every fully-inline world material ("wc/...") found in an inflated zone.
void scan(const std::vector<uint8_t>& zone, Library& out);

bool save(const std::string& path, const Library& lib);
bool load(const std::string& path, Library& lib);

// The common.ff lit technique set for a definition ("" if it has no colorMap), and the textures
// that set samples (colorMap, then normalMap / specularMap if present), in table order.
std::string techsetFor(const Def& d);
std::vector<const Texture*> usedTextures(const Def& d);

std::string key(const std::string& name);

}  // namespace matdef
