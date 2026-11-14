#include "zone/modelbuild.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>

#include "zone/gfxworld.h"

namespace modelbuild {

Quat quatFromAxes(const float a[3][3]) {
    // Rotation matrix with the axes as columns: m[r][c] = axis[c][r].
    const float m00 = a[0][0], m01 = a[1][0], m02 = a[2][0];
    const float m10 = a[0][1], m11 = a[1][1], m12 = a[2][1];
    const float m20 = a[0][2], m21 = a[1][2], m22 = a[2][2];
    Quat q;
    float tr = m00 + m11 + m22;
    if (tr > 0) {
        float s = std::sqrt(tr + 1.0f) * 2;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1.0f + m00 - m11 - m22) * 2;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        float s = std::sqrt(1.0f + m11 - m00 - m22) * 2;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        float s = std::sqrt(1.0f + m22 - m00 - m11) * 2;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l < 1e-9f) return {0, 0, 0, 1};
    for (float& c : q) c /= l;
    return q;
}

Quat quatMul(const Quat& a, const Quat& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
Quat quatConj(const Quat& q) { return {-q[0], -q[1], -q[2], q[3]}; }
V3 quatRotate(const Quat& q, const V3& v) {
    Quat p = quatMul(quatMul(q, {v[0], v[1], v[2], 0}), quatConj(q));
    return {p[0], p[1], p[2]};
}

namespace {

uint16_t toHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    int exp = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
    uint32_t man = x & 0x7FFFFF;
    if (exp <= 0) return static_cast<uint16_t>(sign);
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00);
    return static_cast<uint16_t>(sign | (exp << 10) | (man >> 13));
}

uint32_t packColor(const float c[4]) {
    auto b = [](float v) { return static_cast<uint32_t>(std::clamp(std::lround(v * 255.0f), 0L, 255L)); };
    return b(c[0]) | (b(c[1]) << 8) | (b(c[2]) << 16) | (b(c[3]) << 24);
}

using Influences = std::vector<std::pair<int, float>>;  // reordered bone index, weight (desc, <= 4)

}  // namespace

bool build(const xmodelexport::Model& src, const std::string& name, Model& out, std::string& error,
           const BuildOptions& options) {
    out = Model{};
    out.name = name;
    for (const xmodelexport::Material& m : src.materials) out.materials.push_back({m.name, m.texture});
    if (out.materials.empty()) out.materials.push_back({"default", ""});

    // Bones: roots first, then every bone after its parent (stable otherwise).
    const size_t nb = src.bones.size();
    std::vector<int> order, newIndex(nb, -1);
    for (size_t i = 0; i < nb; i++)
        if (src.bones[i].parent < 0 || src.bones[i].parent >= int(nb)) {
            newIndex[i] = int(order.size());
            order.push_back(int(i));
        }
    out.numRootBones = std::max<int>(1, static_cast<int>(order.size()));
    for (bool progress = true; progress && order.size() < nb;) {
        progress = false;
        for (size_t i = 0; i < nb; i++)
            if (newIndex[i] < 0 && newIndex[src.bones[i].parent] >= 0) {
                newIndex[i] = int(order.size());
                order.push_back(int(i));
                progress = true;
            }
    }
    if (order.size() != nb) {
        error = name + ": bone hierarchy has a cycle";
        return false;
    }
    if (nb > 128) {
        error = name + ": more than 128 bones";
        return false;
    }
    for (int oi : order) {
        const xmodelexport::Bone& b = src.bones[oi];
        Bone bone;
        bone.name = b.name.empty() ? "tag_origin" : b.name;
        // The game's bone names are lowercase (weapon/script anims look bones up by exact string).
        for (char& c : bone.name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bone.parent = (b.parent >= 0 && b.parent < int(nb)) ? newIndex[b.parent] : -1;
        bone.bindQuat = quatFromAxes(b.axis);
        bone.bindTrans = {b.offset[0], b.offset[1], b.offset[2]};
        if (bone.parent >= 0) {
            const Bone& p = out.bones[bone.parent];
            Quat inv = quatConj(p.bindQuat);
            bone.localQuat = quatMul(inv, bone.bindQuat);
            bone.localTrans = quatRotate(inv, {bone.bindTrans[0] - p.bindTrans[0], bone.bindTrans[1] - p.bindTrans[1],
                                               bone.bindTrans[2] - p.bindTrans[2]});
            if (newIndex[oi] - bone.parent > 255) {
                error = name + ": a bone is too far from its parent in the bone list";
                return false;
            }
        } else {
            bone.localQuat = bone.bindQuat;
            bone.localTrans = bone.bindTrans;
        }
        out.bones.push_back(bone);
    }
    out.viewmodel = options.zeroTrans || name.rfind("viewhands", 0) == 0 || name.rfind("viewmodel", 0) == 0;
    for (Bone& b : out.bones) b.modelTrans = out.viewmodel ? V3{} : b.localTrans;
    out.rootBone = out.bones[0].name;
    out.skinned = out.bones.size() > 1;

    // Per-vertex influences (reordered bone indices, top 4, renormalised).
    std::vector<Influences> infl(src.verts.size());
    for (size_t v = 0; v < src.verts.size(); v++) {
        Influences w;
        for (const auto& [b, wt] : src.verts[v].weights)
            if (b >= 0 && b < int(nb) && wt > 0) w.push_back({newIndex[b], wt});
        if (w.empty()) w.push_back({0, 1.0f});
        std::sort(w.begin(), w.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if (w.size() > 4) w.resize(4);
        float sum = 0;
        for (const auto& x : w) sum += x.second;
        for (auto& x : w) x.second /= sum;
        infl[v] = std::move(w);
    }

    for (int k = 0; k < 3; k++) out.mins[k] = 1e30f, out.maxs[k] = -1e30f;
    // Hidden bones (and everything under them): faces whose vertices all hang off them go.
    std::vector<bool> hidden(out.bones.size(), false);
    for (size_t b = 0; b < out.bones.size(); b++) {
        for (const std::string& h : options.hideBones) hidden[b] = hidden[b] || out.bones[b].name == h;
        if (out.bones[b].parent >= 0 && hidden[out.bones[b].parent]) hidden[b] = true;
    }
    size_t droppedHidden = 0, droppedDecal = 0;
    std::map<int, std::vector<size_t>> byMaterial;
    for (size_t i = 0; i < src.faces.size(); i++) {
        int m = src.faces[i].material;
        if (m < 0 || m >= int(out.materials.size())) m = 0;
        // BO1 weapons carry runtime decal slots (clan tag, player emblem): untextured or named so.
        if (options.dropUntextured && (out.materials[m].texture.empty() || out.materials[m].name == "mtl_clan_tag" ||
                                       out.materials[m].name == "mtl_player_icon")) {
            droppedDecal++;
            continue;
        }
        bool allHidden = !options.hideBones.empty();
        for (int k = 0; k < 3 && allHidden; k++) {
            const int v = src.faces[i].v[k].vert;
            allHidden = v >= 0 && v < int(infl.size()) && hidden[infl[v][0].first];
        }
        if (allHidden) {
            droppedHidden++;
            continue;
        }
        byMaterial[m].push_back(i);
    }
    if (droppedHidden)
        out.warnings.push_back(name + ": left out " + std::to_string(droppedHidden) + " triangles on hidden tags");
    if (droppedDecal)
        out.warnings.push_back(name + ": left out " + std::to_string(droppedDecal) + " triangles with untextured materials");
    std::vector<std::vector<int>> surfVertSource;  // per surface: source vertex of each packed vertex
    std::vector<std::vector<std::pair<Vec3, Vec3>>> surfTangents;  // per packed vertex: summed tangent, bitangent
    for (auto& [matIndex, faces] : byMaterial) {
        Surface* s = nullptr;
        std::vector<int>* srcOf = nullptr;
        std::vector<std::pair<Vec3, Vec3>>* tanOf = nullptr;
        std::map<std::tuple<int, int, int, int>, uint16_t> dedupe;
        for (size_t fi : faces) {
            const xmodelexport::Face& f = src.faces[fi];
            if (!s || s->verts.size() + 3 > 65535) {
                out.surfaces.push_back(Surface{});
                s = &out.surfaces.back();
                s->material = matIndex;
                surfVertSource.emplace_back();
                srcOf = &surfVertSource.back();
                surfTangents.emplace_back();
                tanOf = &surfTangents.back();
                dedupe.clear();
            }
            const float* p[3];
            for (int k = 0; k < 3; k++) p[k] = src.verts[f.v[k].vert].pos;
            Vec3 a{p[0][0], p[0][1], p[0][2]}, b{p[1][0], p[1][1], p[1][2]}, c{p[2][0], p[2][1], p[2][2]};
            Vec3 geo = cross(b - a, c - a);
            Vec3 avg;
            for (int k = 0; k < 3; k++) avg += Vec3{f.v[k].normal[0], f.v[k].normal[1], f.v[k].normal[2]};
            if (length(geo) < 1e-9) continue;
            // Game winding: clockwise seen from the front (cross(b-a, c-a) against the normal).
            int ord[3] = {0, 1, 2};
            if (dot(geo, avg) > 0) std::swap(ord[1], ord[2]);
            const xmodelexport::FaceVertex& v0 = f.v[ord[0]];
            const xmodelexport::FaceVertex& v1 = f.v[ord[1]];
            const xmodelexport::FaceVertex& v2 = f.v[ord[2]];
            const float* q0 = src.verts[v0.vert].pos;
            const float* q1 = src.verts[v1.vert].pos;
            const float* q2 = src.verts[v2.vert].pos;
            Vec3 e1{q1[0] - q0[0], q1[1] - q0[1], q1[2] - q0[2]}, e2{q2[0] - q0[0], q2[1] - q0[1], q2[2] - q0[2]};
            double du1 = v1.uv[0] - v0.uv[0], dv1 = v1.uv[1] - v0.uv[1], du2 = v2.uv[0] - v0.uv[0], dv2 = v2.uv[1] - v0.uv[1];
            double det = du1 * dv2 - du2 * dv1;
            // Area-weighted (unnormalised cross) per-face tangent frame, summed per vertex below.
            const double area = length(cross(e1, e2));
            Vec3 tangent = (std::fabs(det) > 1e-12 ? normalize((e1 * dv2 - e2 * dv1) * (1.0 / det)) : normalize(e1)) * area;
            Vec3 bitangent = (std::fabs(det) > 1e-12 ? normalize((e2 * du1 - e1 * du2) * (1.0 / det)) : normalize(e2)) * area;
            for (int k = 0; k < 3; k++) {
                const xmodelexport::FaceVertex& fv = f.v[ord[k]];
                const float* pos = src.verts[fv.vert].pos;
                Vec3 n = normalize(Vec3{fv.normal[0], fv.normal[1], fv.normal[2]});
                PackedVertex pv{};
                std::memcpy(pv.xyz, pos, 12);
                pv.color = packColor(fv.color);
                pv.texCoord = toHalf(fv.uv[0]) | (uint32_t(toHalf(fv.uv[1])) << 16);
                pv.normal = gfx::packUnitVec(n);
                // Mirrored UV islands keep separate vertices (opposite binormal sign).
                const int mirrored = dot(cross(n, tangent), bitangent) < 0;
                auto key = std::make_tuple(fv.vert, int(pv.normal), int(pv.texCoord), int(pv.color) * 2 + mirrored);
                auto it = dedupe.find(key);
                uint16_t idx;
                if (it != dedupe.end()) {
                    idx = it->second;
                    (*tanOf)[idx].first += tangent;
                    (*tanOf)[idx].second += bitangent;
                } else {
                    tanOf->push_back({tangent, bitangent});
                    idx = static_cast<uint16_t>(s->verts.size());
                    s->verts.push_back(pv);
                    srcOf->push_back(fv.vert);
                    dedupe.emplace(key, idx);
                    for (int j = 0; j < 3; j++) {
                        out.mins[j] = std::min(out.mins[j], pos[j]);
                        out.maxs[j] = std::max(out.maxs[j], pos[j]);
                    }
                }
                s->tris.push_back(idx);
            }
        }
        if (s && s->tris.empty()) {
            out.surfaces.pop_back();
            surfVertSource.pop_back();
            surfTangents.pop_back();
        }
    }
    // Finish tangents: Gram-Schmidt the summed tangent against the normal, sign from the bitangent.
    for (size_t si = 0; si < out.surfaces.size(); si++)
        for (size_t v = 0; v < out.surfaces[si].verts.size(); v++) {
            PackedVertex& pv = out.surfaces[si].verts[v];
            Vec3 n = gfx::unpackUnitVec(pv.normal);
            auto [tsum, bsum] = surfTangents[si][v];
            Vec3 t = tsum - n * dot(n, tsum);
            if (length(t) < 1e-9) t = std::fabs(n.x) < 0.9 ? cross(n, Vec3{1, 0, 0}) : cross(n, Vec3{0, 1, 0});
            t = normalize(t);
            pv.tangent = gfx::packUnitVec(t);
            pv.binormalSign = dot(cross(n, t), bsum) < 0 ? -1.0f : 1.0f;
        }
    if (out.surfaces.empty()) {
        error = name + ": no usable triangles";
        return false;
    }

    // Skinning: regroup each surface's vertices by influence count and encode vertsBlend.
    std::vector<AABB> boneBox(out.bones.size());
    for (size_t si = 0; si < out.surfaces.size(); si++) {
        Surface& s = out.surfaces[si];
        const std::vector<int>& srcOf = surfVertSource[si];
        for (size_t v = 0; v < s.verts.size(); v++)
            for (const auto& [b, w] : infl[srcOf[v]]) s.partBits[b >> 5] |= 0x80000000u >> (b & 31);
        if (!out.skinned) continue;
        std::vector<uint16_t> perm;  // new order -> old index
        for (size_t count = 1; count <= 4; count++)
            for (size_t v = 0; v < s.verts.size(); v++)
                if (infl[srcOf[v]].size() == count) {
                    perm.push_back(static_cast<uint16_t>(v));
                    s.blendCounts[count - 1]++;
                }
        std::vector<uint16_t> newOf(s.verts.size());
        std::vector<PackedVertex> verts;
        for (size_t i = 0; i < perm.size(); i++) {
            newOf[perm[i]] = static_cast<uint16_t>(i);
            verts.push_back(s.verts[perm[i]]);
            const Influences& w = infl[srcOf[perm[i]]];
            s.blend.push_back(static_cast<uint16_t>(w[0].first * 64));
            for (size_t k = 1; k < w.size(); k++) {
                s.blend.push_back(static_cast<uint16_t>(w[k].first * 64));
                s.blend.push_back(static_cast<uint16_t>(std::clamp(std::lround(w[k].second * 65536.0f), 0L, 65535L)));
            }
            // bone-space bounds for the dominant bone
            const Bone& bone = out.bones[w[0].first];
            V3 local = quatRotate(quatConj(bone.bindQuat), {s.verts[perm[i]].xyz[0] - bone.bindTrans[0],
                                                            s.verts[perm[i]].xyz[1] - bone.bindTrans[1],
                                                            s.verts[perm[i]].xyz[2] - bone.bindTrans[2]});
            boneBox[w[0].first].add(Vec3{local[0], local[1], local[2]});
        }
        s.verts = std::move(verts);
        for (uint16_t& t : s.tris) t = newOf[t];
    }
    for (size_t b = 0; b < out.bones.size(); b++) {
        Bone& bone = out.bones[b];
        if (!boneBox[b].valid()) continue;
        float r2 = 0;
        for (int k = 0; k < 3; k++) {
            bone.boundsMid[k] = static_cast<float>((boneBox[b].mins[k] + boneBox[b].maxs[k]) * 0.5);
            bone.boundsHalf[k] = static_cast<float>((boneBox[b].maxs[k] - boneBox[b].mins[k]) * 0.5);
            float far = std::fabs(bone.boundsMid[k]) + bone.boundsHalf[k];
            r2 += far * far;
        }
        bone.radiusSquared = r2;
    }
    float r2 = 0;
    for (const Surface& s : out.surfaces)
        for (const PackedVertex& v : s.verts) r2 = std::max(r2, v.xyz[0] * v.xyz[0] + v.xyz[1] * v.xyz[1] + v.xyz[2] * v.xyz[2]);
    out.radius = std::sqrt(r2);
    return true;
}

}  // namespace modelbuild
