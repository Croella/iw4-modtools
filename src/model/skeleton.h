#pragma once
#include <string>
#include <vector>

#include "model/xmodelexport.h"

// Makes a Black Ops (T5) or CoD4 xmodel_export skeleton usable by IW4 (MW2):
//   - bone names lower case (IW4 looks bones up by exact lower-case script strings)
//   - BO1 names IW4 spells differently renamed (j_brow_out_* -> j_browouter_*)
//   - characters (j_mainroot + j_spine4): rooted at tag_origin, and the tags IW4's AI, weapons and
//     scripts attach to added where missing (tag_eye, tag_weapon_right/left, tag_inhand,
//     tag_weapon_chest, tag_sync), placed on their stock parents
//   - at most 128 bones (IW4's limit; BO1 characters carry ~147 with their face rigs): bones IW4
//     never animates (not in skeleton::kIw4Bones, the names MW2's common.ff uses) are removed,
//     unweighted ones first, their vertex weights merged into the nearest kept ancestor and their
//     children re-parented to it. Bind poses are absolute, so nothing else moves.
namespace skeleton {

enum class Source { Unknown, Bo1, Cod4, Iw4 };
const char* sourceName(Source s);
// From the export's "// Export filename" / "Source filename" comment lines (t5 / cod4 paths).
Source detectSource(const std::string& exportText);

struct Report {
    Source source = Source::Unknown;
    std::string kind;  // "character", "viewhands", "model"
    size_t bonesBefore = 0, bonesAfter = 0;
    size_t lowercased = 0;
    std::vector<std::string> renamed, added, removed;
    bool changed() const { return lowercased || !renamed.empty() || !added.empty() || !removed.empty(); }
    std::string summary() const;
};

constexpr size_t kMaxBones = 128;
bool isIw4Bone(const std::string& lowerName);
// Converts in place. False (with error) if the skeleton still can't fit IW4.
bool convertForIw4(xmodelexport::Model& m, Report& report, std::string& error);

}  // namespace skeleton
