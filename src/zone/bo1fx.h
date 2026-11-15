#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Effect materials and their images from a Black Ops (BO1) install, so BO1's raw/fx/*.efx can be
// dropped into zone_source/fx as-is. BO1 ships its effect materials as compiled raw files
// (raw/materials/<name>) and the images in main/iw_*.iwd as IWI version 13 (DXT).
//
// raw/materials/<name> layout (read from BO1's effect materials; offsets are into the file):
//   +0  u32 name offset, +4 u32 colour-image name offset
//   +9  sortKey, +10 atlas rows, +11 atlas columns (as IW4 MaterialInfo)
//   +40 u32 first state-bits word: low byte 0x22 = ONE/ONE (additive), 0x65 = SRCALPHA/INVSRCALPHA
//   +48 u16 textureCount, +50 u16 constantCount, +52 u32 technique-set name offset ("effect",
//       "effect_add_nofog", "effect_zfeather", "particle_cloud_add", ...), +56 texture table
//       offset (12 each: name offset, sampler/semantic, image name offset), +60 constant table
//       offset (20 each: name offset, float4)
namespace bo1 {

struct EffectMaterial {
    std::string name, image, techset;  // techset: BO1's name
    bool additive = false;
    bool fog = true;
    bool cloud = false;   // particle_cloud sets
    float feather = 0;    // featherParms depth (0 = none)
    int rows = 1, cols = 1;
};

struct Image {
    uint32_t format = 0;  // D3DFORMAT (DXT1/3/5 FourCC, or A8R8G8B8)
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;  // top level
};

class Install {
public:
    // root: the Black Ops folder (with raw/ and main/). Empty or missing: everything fails.
    explicit Install(std::string root);
    bool valid() const { return valid_; }
    const std::string& root() const { return root_; }
    bool material(const std::string& name, EffectMaterial& out) const;
    // Just the technique-set name of any raw material (effect or not).
    bool materialTechset(const std::string& name, std::string& techset) const;
    bool image(const std::string& name, Image& out, std::string& error);

private:
    std::string root_;
    bool valid_ = false;
    std::map<std::string, std::pair<std::string, unsigned>> index_;  // images/<name>.iwi -> iwd, file
    bool indexed_ = false;
    void buildIndex();
};

// Where BO1 is: IW4R_BO1 if set, else "Call of Duty Black Ops" next to the MW2 game folder.
std::string defaultRoot(const std::string& mw2GameDir);

}  // namespace bo1
