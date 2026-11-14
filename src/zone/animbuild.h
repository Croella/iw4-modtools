#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "model/xanimexport.h"
#include "zone/modelbuild.h"

// xanim_export -> XAnimParts data, in the encoding the anim evaluator reads (iw4mp 0x2b4d30, for
// numframes < 256; verified against stock juggernaut anims): bones ordered by rotation type
// (boneCount[0..4]: no-quat, half-quat, full-quat, half-quat-no-size, full-quat-no-size), then
// translation parts referencing them by index (boneCount[5..8]: small-trans, trans,
// trans-no-size, no-trans). Keyed bones store a key count - 1 (dataShort), key frame numbers
// 0..numframes (dataByte) and keys (randomDataShort: int16 quats /32767, u16 translations as
// min + key * size with min/size floats in dataInt). Constant bones store their value directly.
//
// xanim_export poses are model space; local (parent-relative) poses use the skeleton of the
// zone_source model that has the anim's bones (parents missing from the anim use its bind pose).
namespace animbuild {

struct Anim {
    std::string name;
    uint16_t numframes = 1;  // frames - 1
    float framerate = 30, frequency = 30;
    uint8_t flags = 0;  // 1 = looping
    uint8_t boneCount[10] = {};
    std::vector<std::string> names;  // part names, in rotation-type order
    std::vector<uint8_t> dataByte;
    std::vector<int16_t> dataShort;
    std::vector<float> dataInt;
    std::vector<int16_t> randomDataShort;
    std::vector<uint8_t> randomDataByte;
    // Notetracks (XAnimNotifyInfo: script string + time 0..1), sorted by time. Always ends with
    // "end" at 1.0 like every stock anim: weapon code reads notify[] without checking the count
    // (iw4sp 0x29c970 / 0x29d225).
    std::vector<std::pair<std::string, float>> notes;
};

// loop: the anim repeats (XAnimParts flag 1); file names ending in "_loop" or "_idle" get it.
// skeletons: zone_source models in order of preference; each part's parent comes from the first
// one that has the bone (so hands + gun anims find both rigs). Anims longer than 256 frames are
// resampled to 256 (framerate scaled to keep the length).
bool build(const xanimexport::Anim& src, const std::string& name, const std::vector<const modelbuild::Model*>& skeletons,
           bool loop, Anim& out, std::string& error);

}  // namespace animbuild
