#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The pieces of a stock sky that a compiled map must carry itself: sky materials, their "wc_sky"
// technique set and its shaders only ship inside each map's own zone (none are in common/code
// zones), so a map can't reference them by name. They are copied from the installed SP
// trainer.ff (unsigned) once and cached; the cubemap pixels stay in the game's IWDs
// (images/sky_*_ft.iwi) and stream in by image name.
//
// What we take (raw stock bytes, x64 layouts as Load_* reads them):
//   - technique slot 4 ("sky", the only one that draws): 16-byte header, one 40-byte pass,
//     vertex + pixel shader structs (0x20) and bytecode (sky.hlsl: vs = position only, w=0 so the
//     sky sits at infinity; ps = cube texld on skyMapSampler s4, squared), pass args + literals.
//     The stock pass's vertex declaration is a back-reference into the zone, so it's rebuilt
//     ("pp": one stream, position -> position). The shared depth/shadow techniques (slots 9-36,
//     44) are back-references too and are left out: the sky never needs them.
//   - material struct (0x78: sort key, state flags, camera region), its texture def (colorMap
//     hash/sampler), and state bits table.
//   - image struct (0x28) + 16-byte load def (resourceSize 0 = load from file).
namespace sky {

struct ShaderBlob {
    std::vector<uint8_t> header;  // 0x20
    std::vector<uint8_t> code;
};

struct Source {
    std::vector<uint8_t> technique;  // 16-byte MaterialTechnique header (slot 4)
    std::vector<uint8_t> pass;       // 40-byte MaterialPass
    ShaderBlob vs, ps;
    std::vector<uint8_t> args;                   // 16 bytes per argument
    std::vector<std::vector<uint8_t>> literals;  // per argument: 16 bytes, or empty
    std::vector<uint8_t> techset;                // 0x198 MaterialTechniqueSet (for its header fields)
    std::vector<uint8_t> material;               // 0x78
    std::vector<uint8_t> textureDef;             // 16
    std::vector<uint8_t> stateBits;              // 8 per entry
    std::vector<uint8_t> image;                  // 0x28
    std::vector<uint8_t> imageLoadDef;           // 16
    bool valid() const { return technique.size() == 16 && !vs.code.empty() && !ps.code.empty(); }
};

// Parses an inflated stock map zone (one that has wc_sky + a wc/sky_* material).
bool extract(const std::vector<uint8_t>& zone, Source& out, std::string& error);

// Cached at <cacheDir>/sky_source.bin; on a miss inflates <gameDir>/zone/english/trainer.ff.
bool load(const std::string& gameDir, const std::string& cacheDir, Source& out, std::string& error);

// Sky image (cubemap streamed from images/<name>.iwi) for a sky material name: "sky_mp_boneyard"
// or "wc/sky_mp_boneyard" -> "sky_mp_boneyard_ft". Plain "sky" -> fallback.
std::string imageForMaterial(const std::string& material, const std::string& fallback = "sky_sp_trainer_ft");

}  // namespace sky
