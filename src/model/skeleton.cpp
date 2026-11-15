#include "model/skeleton.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include "model/iw4bones.h"

namespace skeleton {
namespace {
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
int find(const xmodelexport::Model& m, const std::string& name) {
    for (size_t i = 0; i < m.bones.size(); i++)
        if (m.bones[i].name == name) return static_cast<int>(i);
    return -1;
}
}  // namespace

const char* sourceName(Source s) {
    switch (s) {
        case Source::Bo1: return "Black Ops";
        case Source::Cod4: return "CoD4";
        case Source::Iw4: return "MW2";
        default: return "unknown";
    }
}

Source detectSource(const std::string& text) {
    const std::string head = lower(text.substr(0, 1024));
    if (head.find("/t5/") != std::string::npos || head.find("black ops") != std::string::npos) return Source::Bo1;
    if (head.find("call of duty 4") != std::string::npos || head.find("/cod4/") != std::string::npos) return Source::Cod4;
    if (head.find("/iw4/") != std::string::npos || head.find("modern warfare 2") != std::string::npos) return Source::Iw4;
    return Source::Unknown;
}

bool isIw4Bone(const std::string& name) {
    static const std::set<std::string> set(std::begin(kIw4Bones), std::end(kIw4Bones));
    return set.count(name) != 0;
}

std::string Report::summary() const {
    std::string s = std::string(sourceName(source)) + " " + kind + ": " + std::to_string(bonesBefore) + " -> " +
                    std::to_string(bonesAfter) + " bones";
    if (lowercased) s += ", " + std::to_string(lowercased) + " lower-cased";
    if (!renamed.empty()) s += ", " + std::to_string(renamed.size()) + " renamed";
    if (!added.empty()) {
        s += ", added";
        for (const std::string& a : added) s += " " + a;
    }
    if (!removed.empty()) s += ", " + std::to_string(removed.size()) + " removed (not used by MW2)";
    if (!changed()) s += ", already IW4-ready";
    return s;
}

bool convertForIw4(xmodelexport::Model& m, Report& r, std::string& error) {
    r.bonesBefore = m.bones.size();
    // Names: lower case, then BO1 spellings IW4 has under another name.
    static const std::map<std::string, std::string> kRenames = {
        {"j_brow_out_le", "j_browouter_le"},
        {"j_brow_out_ri", "j_browouter_ri"},
    };
    for (auto& b : m.bones) {
        std::string l = lower(b.name);
        if (l != b.name) r.lowercased++;
        b.name = l;
        auto it = kRenames.find(b.name);
        if (it != kRenames.end() && find(m, it->second) < 0) {
            r.renamed.push_back(b.name + " -> " + it->second);
            b.name = it->second;
        }
    }
    const bool character = find(m, "j_mainroot") >= 0 && find(m, "j_spine4") >= 0;
    const bool viewhands = !character && find(m, "tag_view") >= 0 && (find(m, "j_wrist_ri") >= 0 || find(m, "j_wrist_le") >= 0);
    r.kind = character ? "character" : viewhands ? "viewhands" : "model";
    auto addBone = [&](const std::string& name, int parent, const float* pos) {
        xmodelexport::Bone nb;
        nb.name = name;
        nb.parent = parent;
        if (parent >= 0) {
            const xmodelexport::Bone& p = m.bones[parent];
            for (int k = 0; k < 3; k++)
                for (int j = 0; j < 3; j++) nb.axis[k][j] = p.axis[k][j];
            for (int k = 0; k < 3; k++) nb.offset[k] = pos ? pos[k] : p.offset[k];
        }
        m.bones.push_back(nb);
        r.added.push_back(name);
        return static_cast<int>(m.bones.size()) - 1;
    };
    if (character) {
        // Root: tag_origin (IW4 AI/anims are rooted there). If the root has another name, a new
        // tag_origin at the model origin becomes the parent of every old root.
        if (find(m, "tag_origin") < 0) {
            xmodelexport::Bone root;
            root.name = "tag_origin";
            m.bones.insert(m.bones.begin(), root);
            for (size_t i = 1; i < m.bones.size(); i++) m.bones[i].parent = m.bones[i].parent < 0 ? 0 : m.bones[i].parent + 1;
            for (auto& v : m.verts)
                for (auto& w : v.weights) w.first++;
            r.added.push_back("tag_origin");
        }
        // Tags IW4 code and scripts attach to, on their stock parents.
        const int head = find(m, "j_head");
        if (find(m, "tag_eye") < 0 && head >= 0) {
            const int el = find(m, "j_eyeball_le"), er = find(m, "j_eyeball_ri");
            float pos[3];
            if (el >= 0 && er >= 0) {
                for (int k = 0; k < 3; k++) pos[k] = (m.bones[el].offset[k] + m.bones[er].offset[k]) * 0.5f;
            } else {  // ~ eye height in front of the head joint, along its axes
                const auto& h = m.bones[head];
                for (int k = 0; k < 3; k++) pos[k] = h.offset[k] + h.axis[0][k] * 3.5f + h.axis[1][k] * 0.0f + h.axis[2][k] * 0.0f;
            }
            addBone("tag_eye", head, pos);
        }
        const std::pair<const char*, const char*> kTags[] = {
            {"tag_weapon_right", "j_wrist_ri"}, {"tag_weapon_left", "j_wrist_le"}, {"tag_inhand", "j_wrist_ri"},
            {"tag_weapon_chest", "j_spine4"},   {"tag_sync", "tag_origin"},
        };
        for (const auto& [tag, parent] : kTags) {
            const int p = find(m, parent);
            if (find(m, tag) < 0 && p >= 0) addBone(tag, p, nullptr);
        }
    }
    // Bone limit: remove bones MW2 never names, unweighted first, then the least-weighted.
    if (m.bones.size() > kMaxBones) {
        std::vector<size_t> weightCount(m.bones.size(), 0);
        for (const auto& v : m.verts)
            for (const auto& w : v.weights)
                if (w.first >= 0 && w.first < static_cast<int>(m.bones.size()) && w.second > 0) weightCount[w.first]++;
        std::vector<int> candidates;
        for (size_t i = 0; i < m.bones.size(); i++)
            if (m.bones[i].parent >= 0 && !isIw4Bone(m.bones[i].name)) candidates.push_back(static_cast<int>(i));
        std::stable_sort(candidates.begin(), candidates.end(), [&](int a, int b) { return weightCount[a] < weightCount[b]; });
        std::vector<bool> drop(m.bones.size(), false);
        size_t count = m.bones.size();
        for (int c : candidates) {
            if (count <= kMaxBones) break;
            drop[c] = true;
            count--;
        }
        if (count > kMaxBones) {
            error = std::to_string(count) + " bones after removing every bone MW2 doesn't use; IW4 allows " + std::to_string(kMaxBones);
            return false;
        }
        auto keptAncestor = [&](int i) {
            while (i >= 0 && drop[i]) i = m.bones[i].parent;
            return i;
        };
        std::vector<int> newIndex(m.bones.size(), -1);
        std::vector<xmodelexport::Bone> kept;
        for (size_t i = 0; i < m.bones.size(); i++) {
            if (drop[i]) {
                r.removed.push_back(m.bones[i].name);
                continue;
            }
            newIndex[i] = static_cast<int>(kept.size());
            kept.push_back(m.bones[i]);
        }
        for (size_t i = 0, k = 0; i < m.bones.size(); i++) {
            if (drop[i]) continue;
            const int p = keptAncestor(m.bones[i].parent);
            kept[k++].parent = p >= 0 ? newIndex[p] : -1;
        }
        for (auto& v : m.verts) {
            std::map<int, float> merged;
            for (const auto& [b, w] : v.weights) {
                const int a = b >= 0 && b < static_cast<int>(m.bones.size()) ? keptAncestor(b) : -1;
                if (a >= 0) merged[newIndex[a]] += w;
            }
            v.weights.assign(merged.begin(), merged.end());
            if (v.weights.empty()) v.weights.push_back({0, 1.0f});
        }
        m.bones = std::move(kept);
    }
    r.bonesAfter = m.bones.size();
    return true;
}

}  // namespace skeleton
