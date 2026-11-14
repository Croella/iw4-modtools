#include "zone/animbuild.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace animbuild {
namespace {

using modelbuild::Quat;
using modelbuild::V3;

int16_t q16(float v) { return static_cast<int16_t>(std::clamp(std::lround(v * 32767.0f), -32767L, 32767L)); }

enum RotType { kNoQuat = 0, kFullQuat = 2, kFullQuatNoSize = 4 };
enum TransType { kTrans = 6, kTransNoSize = 7, kNoTrans = 8 };

}  // namespace

bool build(const xanimexport::Anim& src, const std::string& name, const std::vector<const modelbuild::Model*>& skeletons,
           bool loop, Anim& out, std::string& error) {
    out = Anim{};
    out.name = name;
    const size_t parts = src.parts.size();
    size_t frames = src.frames.size();
    float framerate = src.framerate > 0 ? src.framerate : 30.0f;
    // Long anims: resample to 256 frames (the short-index encoding), keeping the duration.
    std::vector<const std::vector<xanimexport::PartPose>*> framePtr;
    std::vector<std::vector<xanimexport::PartPose>> resampled;
    if (frames > 256) {
        const size_t n = 256;
        resampled.resize(n);
        for (size_t f = 0; f < n; f++) {
            const double t = double(f) * double(frames - 1) / double(n - 1);
            const size_t a = std::min(frames - 1, static_cast<size_t>(t));
            const size_t b = std::min(frames - 1, a + 1);
            const float w = static_cast<float>(t - double(a));
            resampled[f] = src.frames[a];
            for (size_t p = 0; p < parts; p++) {
                xanimexport::PartPose& o = resampled[f][p];
                const xanimexport::PartPose& pb = src.frames[b][p];
                for (int k = 0; k < 3; k++) {
                    o.offset[k] += (pb.offset[k] - o.offset[k]) * w;
                    for (int j = 0; j < 3; j++) o.axis[k][j] += (pb.axis[k][j] - o.axis[k][j]) * w;  // re-normalised by quatFromAxes
                }
            }
        }
        framerate = framerate * float(n - 1) / float(frames - 1);
        frames = n;
        for (auto& r : resampled) framePtr.push_back(&r);
    } else {
        for (const auto& r : src.frames) framePtr.push_back(&r);
    }
    // Model-space pose per frame/part.
    std::vector<std::vector<Quat>> wq(frames, std::vector<Quat>(parts));
    std::vector<std::vector<V3>> wt(frames, std::vector<V3>(parts));
    for (size_t f = 0; f < frames; f++)
        for (size_t p = 0; p < parts; p++) {
            const xanimexport::PartPose& pp = (*framePtr[f])[p];
            wq[f][p] = modelbuild::quatFromAxes(pp.axis);
            wt[f][p] = {pp.offset[0], pp.offset[1], pp.offset[2]};
        }
    // Parent of each part, from the skeleton: another part (animated) or a skeleton bone (bind).
    std::map<std::string, int> partOf;
    for (size_t p = 0; p < parts; p++) partOf[src.parts[p]] = static_cast<int>(p);
    std::vector<int> parentPart(parts, -1), parentBone(parts, -1), selfBone(parts, -1);
    std::vector<const modelbuild::Model*> skelOf(parts, nullptr);
    for (size_t p = 0; p < parts; p++)
        for (const modelbuild::Model* skeleton : skeletons) {
            int found = -1;
            for (size_t b = 0; b < skeleton->bones.size() && found < 0; b++)
                if (skeleton->bones[b].name == src.parts[p]) found = static_cast<int>(b);
            if (found < 0) continue;
            skelOf[p] = skeleton;
            selfBone[p] = found;
            int parent = skeleton->bones[found].parent;
            if (parent >= 0) {
                auto pp = partOf.find(skeleton->bones[parent].name);
                if (pp != partOf.end()) parentPart[p] = pp->second;
                else parentBone[p] = parent;
            }
            break;
        }
    // Local (parent-relative) pose per frame/part.
    std::vector<std::vector<Quat>> lq(frames, std::vector<Quat>(parts));
    std::vector<std::vector<V3>> lt(frames, std::vector<V3>(parts));
    for (size_t f = 0; f < frames; f++)
        for (size_t p = 0; p < parts; p++) {
            Quat pq{0, 0, 0, 1};
            V3 pt{};
            if (parentPart[p] >= 0) {
                pq = wq[f][parentPart[p]];
                pt = wt[f][parentPart[p]];
            } else if (parentBone[p] >= 0) {
                pq = skelOf[p]->bones[parentBone[p]].bindQuat;
                pt = skelOf[p]->bones[parentBone[p]].bindTrans;
            }
            Quat inv = modelbuild::quatConj(pq);
            lq[f][p] = modelbuild::quatMul(inv, wq[f][p]);
            lt[f][p] = modelbuild::quatRotate(inv, {wt[f][p][0] - pt[0], wt[f][p][1] - pt[1], wt[f][p][2] - pt[2]});
            // The game adds the model's trans to the anim's (see modelbuild::Bone::modelTrans).
            if (selfBone[p] >= 0 && skelOf[p]->bones[selfBone[p]].parent >= 0)
                for (int k = 0; k < 3; k++) lt[f][p][k] -= skelOf[p]->bones[selfBone[p]].modelTrans[k];
        }
    if (frames == 1) {  // a pose: two identical keys
        lq.push_back(lq[0]);
        lt.push_back(lt[0]);
        frames = 2;
    }
    {
        const float last = float(std::max<size_t>(2, src.frames.size()) - 1);
        bool hasEnd = false;
        for (const auto& n : src.notes) {
            std::string nm = n.name;
            for (char& c : nm) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const float t = std::clamp(float(n.frame) / last, 0.0f, 1.0f);
            hasEnd |= nm == "end";
            out.notes.push_back({nm, nm == "end" ? 1.0f : t});
        }
        if (!hasEnd) out.notes.push_back({"end", 1.0f});
        std::stable_sort(out.notes.begin(), out.notes.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    }
    out.numframes = static_cast<uint16_t>(frames - 1);
    out.framerate = framerate;
    out.frequency = out.framerate / out.numframes;
    out.flags = loop ? 1 : 0;

    // Classify each part.
    std::vector<int> rot(parts), trans(parts);
    for (size_t p = 0; p < parts; p++) {
        for (size_t f = 1; f < frames; f++) {  // continuous quats for interpolation
            const Quat& a = lq[f - 1][p];
            Quat& b = lq[f][p];
            if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0)
                for (float& c : b) c = -c;
        }
        bool constQ = true, constT = true;
        for (size_t f = 1; f < frames; f++)
            for (int k = 0; k < 4; k++) {
                if (std::fabs(lq[f][p][k] - lq[0][p][k]) > 1e-4f) constQ = false;
                if (k < 3 && std::fabs(lt[f][p][k] - lt[0][p][k]) > 1e-3f) constT = false;
            }
        const Quat& q0 = lq[0][p];
        const bool identity = std::fabs(q0[0]) < 1e-4f && std::fabs(q0[1]) < 1e-4f && std::fabs(q0[2]) < 1e-4f;
        rot[p] = constQ ? (identity ? kNoQuat : kFullQuatNoSize) : kFullQuat;
        const V3& t0 = lt[0][p];
        const bool zero = std::fabs(t0[0]) < 1e-4f && std::fabs(t0[1]) < 1e-4f && std::fabs(t0[2]) < 1e-4f;
        trans[p] = constT ? (zero ? kNoTrans : kTransNoSize) : kTrans;
    }
    // names[]: parts ordered by rotation type; translation entries index into this order.
    std::vector<int> order;
    for (int type : {kNoQuat, kFullQuat, kFullQuatNoSize})
        for (size_t p = 0; p < parts; p++)
            if (rot[p] == type) order.push_back(static_cast<int>(p));
    std::vector<int> slot(parts);
    for (size_t i = 0; i < order.size(); i++) slot[order[i]] = static_cast<int>(i);
    for (int p : order) {
        out.names.push_back(src.parts[p]);
        out.boneCount[rot[p]]++;
    }
    out.boneCount[9] = static_cast<uint8_t>(parts);
    const uint16_t n = out.numframes;  // keys - 1 for keyed parts (every frame is a key)
    // Rotations (half-quat groups are empty).
    for (int p : order)
        if (rot[p] == kFullQuat) {
            out.dataShort.push_back(static_cast<int16_t>(n));
            for (uint16_t f = 0; f <= n; f++) out.dataByte.push_back(static_cast<uint8_t>(f));
            for (size_t f = 0; f < frames; f++)
                for (int k = 0; k < 4; k++) out.randomDataShort.push_back(q16(lq[f][p][k]));
        }
    for (int p : order)
        if (rot[p] == kFullQuatNoSize)
            for (int k = 0; k < 4; k++) out.dataShort.push_back(q16(lq[0][p][k]));
    // Translations, grouped: keyed (u16), constant, none.
    for (int type : {kTrans, kTransNoSize, kNoTrans})
        for (int p : order) {
            if (trans[p] != type) continue;
            out.boneCount[type]++;
            if (type == kTrans) {
                float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
                for (size_t f = 0; f < frames; f++)
                    for (int k = 0; k < 3; k++) mn[k] = std::min(mn[k], lt[f][p][k]), mx[k] = std::max(mx[k], lt[f][p][k]);
                float size[3];
                for (int k = 0; k < 3; k++) size[k] = (mx[k] - mn[k]) / 65535.0f;
                out.dataShort.push_back(static_cast<int16_t>(n));
                out.dataByte.push_back(static_cast<uint8_t>(slot[p]));
                for (uint16_t f = 0; f <= n; f++) out.dataByte.push_back(static_cast<uint8_t>(f));
                out.dataInt.insert(out.dataInt.end(), {mn[0], mn[1], mn[2], size[0], size[1], size[2]});
                for (size_t f = 0; f < frames; f++)
                    for (int k = 0; k < 3; k++) {
                        float v = size[k] > 0 ? (lt[f][p][k] - mn[k]) / size[k] : 0.0f;
                        out.randomDataShort.push_back(static_cast<int16_t>(static_cast<uint16_t>(std::clamp(std::lround(v), 0L, 65535L))));
                    }
            } else if (type == kTransNoSize) {
                out.dataByte.push_back(static_cast<uint8_t>(slot[p]));
                out.dataInt.insert(out.dataInt.end(), {lt[0][p][0], lt[0][p][1], lt[0][p][2]});
            } else {
                out.dataByte.push_back(static_cast<uint8_t>(slot[p]));
            }
        }
    if (out.dataByte.size() > 65535 || out.dataShort.size() > 65535 || out.dataInt.size() > 65535) {
        error = name + ": animation too large";
        return false;
    }
    return true;
}

}  // namespace animbuild
