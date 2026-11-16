#include "mapimport.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include "fastfile.h"
#include "iw4.h"
#include "gfxworld.h"
#include "zonesim.h"

namespace mapimport {
namespace {

constexpr uint64_t kFollow = ~0ull, kInsert = ~1ull;
constexpr uint32_t kClipMap = 15, kMapEnts = 19, kGfxWorld = 21;

uint64_t rd64(const std::vector<uint8_t>& z, size_t o) {
    uint64_t v = 0;
    if (o + 8 <= z.size()) std::memcpy(&v, z.data() + o, 8);
    return v;
}
uint32_t rd32(const std::vector<uint8_t>& z, size_t o) {
    uint32_t v = 0;
    if (o + 4 <= z.size()) std::memcpy(&v, z.data() + o, 4);
    return v;
}

// Debug (IW4R_DUMP_SMODELS): how a stock map stores static models, brush models and model collision.
void dumpModels(const iw4::GfxWorld* g, const iw4::clipMap_t* cm, const iw4::MapEnts* me) {
    auto model = [](const iw4::XModel* m) {
        if (!m || !m->name) return;
        std::printf("    xmodel %s: bones %u surfs %u lods %u collLod %d flags %x contents %x collSurfs %d radius %.1f bounds mid %.1f %.1f %.1f half %.1f %.1f %.1f phys %s collmap %p\n",
                    m->name, m->numBones, m->numsurfs, m->numLods, static_cast<signed char>(m->collLod), m->flags, m->contents, m->numCollSurfs,
                    m->radius, m->bounds.midPoint[0], m->bounds.midPoint[1], m->bounds.midPoint[2], m->bounds.halfSize[0], m->bounds.halfSize[1],
                    m->bounds.halfSize[2], m->physPreset && m->physPreset->name ? m->physPreset->name : "-", static_cast<const void*>(m->physCollmap));
        for (int i = 0; i < m->numCollSurfs && i < 3 && m->collSurfs; i++) {
            const iw4::XModelCollSurf_s& c = m->collSurfs[i];
            std::printf("      collSurf %d: tris %d bone %d contents %x surfFlags %x mid %.2f %.2f %.2f half %.2f %.2f %.2f\n", i, c.numCollTris,
                        c.boneIdx, c.contents, c.surfFlags, c.bounds.midPoint[0], c.bounds.midPoint[1], c.bounds.midPoint[2], c.bounds.halfSize[0],
                        c.bounds.halfSize[1], c.bounds.halfSize[2]);
            for (int t = 0; t < c.numCollTris && t < 8 && c.collTris; t++) {
                const iw4::XModelCollTri_s& r = c.collTris[t];
                std::printf("        tri %d plane %.3f %.3f %.3f %.2f svec %.4f %.4f %.4f %.3f tvec %.4f %.4f %.4f %.3f\n", t, r.plane[0], r.plane[1],
                            r.plane[2], r.plane[3], r.svec[0], r.svec[1], r.svec[2], r.svec[3], r.tvec[0], r.tvec[1], r.tvec[2], r.tvec[3]);
            }
        }
        for (int l = 0; l < 4; l++)
            std::printf("      lod %d dist %.0f surfs %u surfIndex %u lod %d smc %d %d %d\n", l, m->lodInfo[l].dist, m->lodInfo[l].numsurfs,
                        m->lodInfo[l].surfIndex, m->lodInfo[l].lod, m->lodInfo[l].smcBaseIndexPlusOne, m->lodInfo[l].smcSubIndexMask,
                        m->lodInfo[l].smcBucket);
    };
    if (g) {
        const iw4::GfxWorldDpvsStatic& v = g->dpvs;
        std::printf("smodels: %u visDataCount %u surfaces %u models %d cells %d\n", v.smodelCount, v.smodelVisDataCount, g->surfaceCount, g->modelCount,
                    g->dpvsPlanes.cellCount);
        std::set<const iw4::XModel*> shown;
        for (unsigned i = 0; i < v.smodelCount && v.smodelDrawInsts; i++) {
            const iw4::GfxStaticModelDrawInst& d = v.smodelDrawInsts[i];
            if (i < 12 || (d.model && shown.size() < 6 && !shown.count(d.model))) {
                const iw4::GfxPackedPlacement& p = d.placement;
                std::printf("  smodel %u: %s origin %.1f %.1f %.1f scale %.3f axis0 %.3f %.3f %.3f axis2 %.3f %.3f %.3f cull %u lightH %u probe %u primary %u flags %x skin %u ground %08x cache %u %u %u %u\n",
                            i, d.model && d.model->name ? d.model->name : "?", p.origin[0], p.origin[1], p.origin[2], p.scale, p.axis[0][0], p.axis[0][1],
                            p.axis[0][2], p.axis[2][0], p.axis[2][1], p.axis[2][2], d.cullDist, d.lightingHandle, d.reflectionProbeIndex,
                            d.primaryLightIndex, d.flags, d.firstMtlSkinIndex, d.groundLighting.packed, d.cacheId[0], d.cacheId[1], d.cacheId[2],
                            d.cacheId[3]);
                if (v.smodelInsts) {
                    const iw4::GfxStaticModelInst& s = v.smodelInsts[i];
                    std::printf("    inst mid %.1f %.1f %.1f half %.1f %.1f %.1f lightingOrigin %.1f %.1f %.1f\n", s.bounds.midPoint[0],
                                s.bounds.midPoint[1], s.bounds.midPoint[2], s.bounds.halfSize[0], s.bounds.halfSize[1], s.bounds.halfSize[2],
                                s.lightingOrigin[0], s.lightingOrigin[1], s.lightingOrigin[2]);
                }
                if (d.model && !shown.count(d.model)) shown.insert(d.model), model(d.model);
            }
        }
        std::map<unsigned, unsigned> flagCount;
        for (unsigned i = 0; i < v.smodelCount && v.smodelDrawInsts; i++) flagCount[v.smodelDrawInsts[i].flags]++;
        for (auto [f, n] : flagCount) std::printf("  flags %x: %u\n", f, n);
        for (int c = 0; c < g->dpvsPlanes.cellCount && c < 2 && g->aabbTrees && g->aabbTreeCounts; c++) {
            const int nt = g->aabbTreeCounts[c].aabbTreeCount;
            unsigned withSm = 0, totalSm = 0;
            for (int k = 0; k < nt; k++)
                totalSm += g->aabbTrees[c].aabbTree[k].smodelIndexCount, withSm += g->aabbTrees[c].aabbTree[k].smodelIndexCount > 0;
            std::printf("  cell %d: %d trees, %u with smodels, %u smodel refs\n", c, nt, withSm, totalSm);
            for (int k = 0; k < nt && k < 6; k++) {
                const iw4::GfxAabbTree& t = g->aabbTrees[c].aabbTree[k];
                std::printf("    tree %d: children %u off %d surfs %u@%u nodecal %u@%u smodels %u:", k, t.childCount, t.childrenOffset, t.surfaceCount,
                            t.startSurfIndex, t.surfaceCountNoDecal, t.startSurfIndexNoDecal, t.smodelIndexCount);
                for (unsigned j = 0; j < t.smodelIndexCount && j < 8 && t.smodelIndexes; j++) std::printf(" %u", t.smodelIndexes[j]);
                std::printf("\n");
            }
        }
        for (unsigned i = 0; i < g->primaryLightCount && i < 4 && g->shadowGeom; i++)
            std::printf("  shadowGeom %u: surfs %u smodels %u\n", i, g->shadowGeom[i].surfaceCount, g->shadowGeom[i].smodelCount);
        for (int i = 0; i < g->modelCount && i < 8 && g->models; i++) {
            const iw4::GfxBrushModel& m = g->models[i];
            std::printf("  gfx model %d: surfs %u start %u nodecal %u mid %.1f %.1f %.1f half %.1f %.1f %.1f writable mid %.1f half %.1f radius %.1f\n", i,
                        m.surfaceCount, m.startSurfIndex, m.surfaceCountNoDecal, m.bounds.midPoint[0], m.bounds.midPoint[1], m.bounds.midPoint[2],
                        m.bounds.halfSize[0], m.bounds.halfSize[1], m.bounds.halfSize[2], m.writable.bounds.midPoint[0], m.writable.bounds.halfSize[0],
                        m.radius);
            if (i > 0 && m.surfaceCount && g->dpvs.surfaces && g->draw.vd.vertices) {
                const iw4::GfxSurface& s = g->dpvs.surfaces[m.startSurfIndex];
                const iw4::GfxWorldVertex& vx = g->draw.vd.vertices[s.tris.firstVertex];
                std::printf("    first surf %s lmap %u primary %u flags %x vert %.1f %.1f %.1f\n",
                            s.material && s.material->info.name ? s.material->info.name : "?", s.laf.fields.lightmapIndex,
                            s.laf.fields.primaryLightIndex, s.laf.fields.flags, vx.xyz[0], vx.xyz[1], vx.xyz[2]);
            }
        }
    }
    if (cm) {
        std::printf("clip static models %u, smodelNodes %u, submodels %u, partitions %u aabbTrees %u verts %u tris %u leafsurfaces %u\n",
                    cm->numStaticModels, cm->smodelNodeCount, cm->numSubModels, cm->partitionCount, cm->aabbTreeCount, cm->vertCount, cm->triCount,
                    cm->numLeafSurfaces);
        for (unsigned i = 0; i < cm->numStaticModels && i < 6 && cm->staticModelList; i++) {
            const iw4::cStaticModel_s& s = cm->staticModelList[i];
            std::printf("  cstatic %u: %s origin %.1f %.1f %.1f invAxis0 %.4f %.4f %.4f abs mid %.1f %.1f %.1f half %.1f %.1f %.1f\n", i,
                        s.xmodel && s.xmodel->name ? s.xmodel->name : "?", s.origin[0], s.origin[1], s.origin[2], s.invScaledAxis[0][0],
                        s.invScaledAxis[0][1], s.invScaledAxis[0][2], s.absBounds.midPoint[0], s.absBounds.midPoint[1], s.absBounds.midPoint[2],
                        s.absBounds.halfSize[0], s.absBounds.halfSize[1], s.absBounds.halfSize[2]);
            for (int r = 0; r < 3; r++)
                std::printf("    inv[%d] %.4f %.4f %.4f\n", r, s.invScaledAxis[r][0], s.invScaledAxis[r][1], s.invScaledAxis[r][2]);
            for (unsigned j = 0; g && j < g->dpvs.smodelCount && g->dpvs.smodelDrawInsts; j++) {
                const iw4::GfxPackedPlacement& p = g->dpvs.smodelDrawInsts[j].placement;
                if (std::fabs(p.origin[0] - s.origin[0]) + std::fabs(p.origin[1] - s.origin[1]) + std::fabs(p.origin[2] - s.origin[2]) > 0.01f)
                    continue;
                std::printf("    = smodel %u scale %.4f model %s\n", j, p.scale,
                            g->dpvs.smodelDrawInsts[j].model && g->dpvs.smodelDrawInsts[j].model->name ? g->dpvs.smodelDrawInsts[j].model->name : "?");
                for (int r = 0; r < 3; r++) std::printf("    axis[%d] %.4f %.4f %.4f\n", r, p.axis[r][0], p.axis[r][1], p.axis[r][2]);
                break;
            }
        }
        // Static model aabb tree: check the encoding (child index >= numStaticModels -> node).
        unsigned leaves = 0, leafModels = 0, inner = 0, bad = 0;
        for (unsigned i = 0; i < cm->smodelNodeCount && cm->smodelNodes; i++) {
            const iw4::SModelAabbNode& n = cm->smodelNodes[i];
            if (n.firstChild >= cm->numStaticModels) {
                inner++;
                if (n.firstChild - cm->numStaticModels + n.childCount > cm->smodelNodeCount) bad++;
            } else {
                leaves++;
                leafModels += n.childCount;
            }
        }
        std::printf("  smodel tree: %u inner, %u leaves listing %u models (of %u), %u bad\n", inner, leaves, leafModels, cm->numStaticModels, bad);
        for (unsigned i = 0; i < cm->smodelNodeCount && i < 6 && cm->smodelNodes; i++) {
            const uint8_t* n = reinterpret_cast<const uint8_t*>(&cm->smodelNodes[i]);
            std::printf("  smodelNode %u:", i);
            for (size_t b = 0; b + 4 <= sizeof(iw4::SModelAabbNode); b += 4) {
                float f;
                uint32_t u;
                std::memcpy(&f, n + b, 4);
                std::memcpy(&u, n + b, 4);
                std::printf(" %g/%08x", f, u);
            }
            std::printf("\n");
        }
        for (unsigned i = 0; i < cm->numSubModels && i < 8 && cm->cmodels; i++) {
            const iw4::cmodel_t& m = cm->cmodels[i];
            std::printf("  cmodel %u: mid %.1f %.1f %.1f half %.1f %.1f %.1f radius %.1f leaf brushNode %d contents %x terrain %x aabb %u@%u leafmid %.1f %.1f %.1f\n",
                        i, m.bounds.midPoint[0], m.bounds.midPoint[1], m.bounds.midPoint[2], m.bounds.halfSize[0], m.bounds.halfSize[1],
                        m.bounds.halfSize[2], m.radius, m.leaf.leafBrushNode, m.leaf.brushContents, m.leaf.terrainContents, m.leaf.collAabbCount,
                        m.leaf.firstCollAabbIndex, m.leaf.bounds.midPoint[0], m.leaf.bounds.midPoint[1], m.leaf.bounds.midPoint[2]);
            if (i > 0 && cm->leafbrushNodes && m.leaf.leafBrushNode > 0) {
                const iw4::cLeafBrushNode_s& n = cm->leafbrushNodes[m.leaf.leafBrushNode];
                std::printf("    node axis %u count %d contents %x", n.axis, n.leafBrushCount, n.contents);
                if (n.leafBrushCount > 0 && n.data.leaf.brushes && cm->brushBounds) {
                    const unsigned b = n.data.leaf.brushes[0];
                    std::printf(" brush %u mid %.1f %.1f %.1f half %.1f %.1f %.1f", b, cm->brushBounds[b].midPoint[0], cm->brushBounds[b].midPoint[1],
                                cm->brushBounds[b].midPoint[2], cm->brushBounds[b].halfSize[0], cm->brushBounds[b].halfSize[1],
                                cm->brushBounds[b].halfSize[2]);
                }
                std::printf("\n");
            }
        }
    }
    if (me && me->entityString) {  // entities using brush models
        std::istringstream in(me->entityString);
        std::string line, block;
        int shown = 0;
        while (std::getline(in, line) && shown < 8) {
            if (line == "{") block.clear();
            block += line + " ";
            if (line == "}" && block.find("\"model\" \"*") != std::string::npos) std::printf("  ent: %s\n", block.c_str()), shown++;
        }
    }
}

bool isVirtualRef(uint64_t p) { return p && p < (1ull << 32) && ((p - 1) >> 28) == 3; }
uint32_t refOffset(uint64_t p) { return static_cast<uint32_t>((p - 1) & 0x0FFFFFFF); }

// The editor (like .map files) names world materials without their "wc/" asset prefix.
std::string editorMaterialName(std::string n) {
    if (n.rfind("wc/", 0) == 0) n.erase(0, 3);
    return n;
}

// A face on plane (n, d), oriented like makeBox so Brush::rebuild derives the same outward normal.
Face planeFace(const Vec3& n, double d, const Vec3& near, const std::string& material) {
    Vec3 u = std::fabs(n.z) > 0.5 ? Vec3{1, 0, 0} : Vec3{0, 0, 1};
    u = normalize(u - n * dot(u, n));
    Vec3 w = cross(n, u);
    Vec3 o = near - n * (dot(n, near) - d);
    Face f;
    f.p[0] = o;
    f.p[1] = o + w * 64;
    f.p[2] = o + u * 64;
    f.material = material.empty() ? "caulk" : material;
    f.tail = "lightmap_gray 16384 16384 0 0 0 0";
    return f;
}

// One clipMap brush -> an editor brush: the 6 axial planes from its bounds, plus its sides.
bool convertBrush(const iw4::clipMap_t& c, unsigned i, Brush& out) {
    const iw4::cbrush_t& b = c.brushes[i];
    const iw4::Bounds& bb = c.brushBounds[i];
    // clipMap names are already in the editor's (.map) form: world materials without "wc/".
    auto matName = [&](unsigned idx) -> std::string {
        std::string n = idx < c.numMaterials && c.materials[idx].name ? c.materials[idx].name : "";
        if (!n.empty() && n[0] == ',') n.erase(0, 1);
        return editorMaterialName(n);
    };
    Vec3 mid{bb.midPoint[0], bb.midPoint[1], bb.midPoint[2]};
    Brush br;
    for (int axis = 0; axis < 3; axis++)
        for (int side = 0; side < 2; side++) {
            Vec3 n;
            n[axis] = side ? 1 : -1;
            double d = side ? bb.midPoint[axis] + bb.halfSize[axis] : -(bb.midPoint[axis] - bb.halfSize[axis]);
            br.faces.push_back(planeFace(n, d, mid, matName(b.axialMaterialNum[side][axis])));
        }
    for (unsigned k = 0; k < b.numsides && b.sides; k++) {
        const iw4::cbrushside_t& s = b.sides[k];
        if (!s.plane) return false;
        Vec3 n{s.plane->normal[0], s.plane->normal[1], s.plane->normal[2]};
        if (std::fabs(length(n) - 1.0) > 0.01) return false;  // unresolved plane
        br.faces.push_back(planeFace(n, s.plane->dist, mid, matName(s.materialNum)));
    }
    if (!br.rebuild()) return false;
    // Collision brushes carry bevel planes that touch the solid only along an edge; drop them.
    br.faces.erase(std::remove_if(br.faces.begin(), br.faces.end(), [](const Face& f) { return f.winding.size() < 3; }),
                   br.faces.end());
    if (br.faces.size() < 4 || !br.rebuild()) return false;
    out = std::move(br);
    return true;
}

std::shared_ptr<RefMesh> convertGfxWorld(const iw4::GfxWorld& g, ImportStats& st) {
    auto mesh = std::make_shared<RefMesh>();
    const iw4::GfxWorldDraw& d = g.draw;
    if (!d.vd.vertices || !d.indices || !g.dpvs.surfaces) return nullptr;
    mesh->positions.reserve(3ull * d.vertexCount);
    mesh->normals.reserve(3ull * d.vertexCount);
    mesh->uvs.reserve(2ull * d.vertexCount);
    for (unsigned i = 0; i < d.vertexCount; i++) {
        const iw4::GfxWorldVertex& v = d.vd.vertices[i];
        for (int k = 0; k < 3; k++) mesh->positions.push_back(v.xyz[k]);
        uint32_t p = v.normal.packed;
        double scale = (static_cast<double>(p >> 24) + 192.0) / 32385.0;
        for (int k = 0; k < 3; k++) mesh->normals.push_back(static_cast<float>((((p >> (8 * k)) & 0xFF) - 127.0) * scale));
        mesh->uvs.push_back(v.texCoord[0]);
        mesh->uvs.push_back(v.texCoord[1]);
        mesh->bounds.add(Vec3{v.xyz[0], v.xyz[1], v.xyz[2]});
    }
    std::map<std::string, std::vector<uint32_t>> byMaterial;
    for (unsigned i = 0; i < g.surfaceCount; i++) {
        const iw4::GfxSurface& s = g.dpvs.surfaces[i];
        std::string name = s.material && s.material->info.name ? s.material->info.name : "";
        if (!name.empty() && name[0] == ',') name.erase(0, 1);  // by-name reference
        if (!name.empty()) st.surfacesTextured++;
        // The loaded material's colorMap image, so the editor can texture names its own
        // material scan couldn't resolve.
        if (!name.empty() && s.material->textureTable && !mesh->materialImages.count(name))
            for (int t = 0; t < s.material->textureCount; t++) {
                const iw4::MaterialTextureDef& td = s.material->textureTable[t];
                if (td.semantic == iw4::TS_COLOR_MAP && td.u.image && td.u.image->name && td.u.image->name[0]) {
                    std::string img = td.u.image->name;
                    if (img[0] == ',') img.erase(0, 1);
                    mesh->materialImages[name] = img;
                    break;
                }
            }
        name = editorMaterialName(name);
        std::vector<uint32_t>& out = byMaterial[name];
        for (unsigned k = 0; k < 3u * s.tris.triCount; k++) {
            uint64_t ix = static_cast<uint64_t>(s.tris.baseIndex) + k;
            if (ix >= d.indexCount) break;
            uint32_t v = s.tris.firstVertex + d.indices[ix];
            if (v >= d.vertexCount) v = 0;
            out.push_back(v);
        }
        st.triangles += s.tris.triCount;
    }
    st.surfaces = g.surfaceCount;
    for (auto& [name, idx] : byMaterial) {
        mesh->batches.push_back({name, static_cast<uint32_t>(mesh->indices.size()), static_cast<uint32_t>(idx.size())});
        mesh->indices.insert(mesh->indices.end(), idx.begin(), idx.end());
    }
    return mesh;
}

// Where a stock zone keeps its MapEnts: the struct right before the inline entity lump.
bool findMapEnts(const std::vector<uint8_t>& z, size_t& structAt) {
    static const char kWorld[] = "\"classname\" \"worldspawn\"";
    for (auto it = z.begin();;) {
        it = std::search(it, z.end(), kWorld, kWorld + sizeof(kWorld) - 1);
        if (it == z.end()) return false;
        size_t m = static_cast<size_t>(it - z.begin());
        const void* nul = std::memchr(z.data() + m, 0, z.size() - m);
        if (nul) {
            size_t end = static_cast<const uint8_t*>(nul) - z.data();  // the lump's NUL
            for (size_t o = m > 0x58 + 4096 ? m - 0x58 - 4096 : 0; o + 0x58 <= m; o++) {
                if (rd64(z, o + 8) != kFollow) continue;
                uint32_t chars = rd32(z, o + 16);
                if (chars == 0 || chars > end + 1) continue;
                size_t start = end + 1 - chars;
                uint64_t name = rd64(z, o);
                size_t dataAt = o + 0x58;
                if (name == kFollow) {
                    const void* nn = std::memchr(z.data() + dataAt, 0, z.size() - dataAt);
                    if (!nn) continue;
                    dataAt = static_cast<const uint8_t*>(nn) - z.data() + 1;
                }
                if (start == dataAt && start <= m) {
                    structAt = o;
                    return true;
                }
            }
        }
        ++it;
    }
}

std::vector<size_t> findQword(const std::vector<uint8_t>& z, uint64_t value) {
    std::vector<size_t> hits;
    uint8_t key[8];
    std::memcpy(key, &value, 8);
    for (auto it = z.begin();;) {
        it = std::search(it, z.end(), key, key + 8);
        if (it == z.end()) break;
        hits.push_back(static_cast<size_t>(it - z.begin()));
        ++it;
    }
    return hits;
}

void fillDocument(Document& doc, const iw4::MapEnts* me, const iw4::clipMap_t* c, std::shared_ptr<RefMesh> mesh,
                  ImportStats& st, std::string& error) {
    doc.reset();
    if (me && me->entityString) {
        std::vector<Entity> ents;
        if (parseEntityString(std::string(me->entityString, static_cast<size_t>(std::max(0, me->numEntityChars - 1))),
                              ents, error) &&
            !ents.empty())
            doc.ents = std::move(ents);
    }
    if (doc.ents.empty() || doc.ents[0].classname() != "worldspawn") {
        Entity world;
        world.set("classname", "worldspawn");
        doc.ents.insert(doc.ents.begin(), std::move(world));
    }
    st.entities = doc.ents.size();
    if (c && c->brushes && c->brushBounds) {
        for (unsigned i = 0; i < c->numBrushes; i++) {
            Brush b;
            if (convertBrush(*c, i, b)) {
                doc.ents[0].brushes.push_back(std::move(b));
                st.brushes++;
            } else {
                st.brushesSkipped++;
            }
        }
    }
    doc.refMesh = mesh;
    doc.dirty = true;
    doc.touch();
}

}  // namespace

bool parseEntityString(const std::string& text, std::vector<Entity>& out, std::string& error) {
    out.clear();
    size_t i = 0;
    auto skipWs = [&] {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) i++;
    };
    auto quoted = [&](std::string& s) {
        if (i >= text.size() || text[i] != '"') return false;
        size_t e = text.find('"', i + 1);
        if (e == std::string::npos) return false;
        s = text.substr(i + 1, e - i - 1);
        i = e + 1;
        return true;
    };
    for (;;) {
        skipWs();
        if (i >= text.size()) return true;
        if (text[i] != '{') {
            error = "entity lump: expected '{' at " + std::to_string(i);
            return false;
        }
        i++;
        Entity e;
        for (;;) {
            skipWs();
            if (i < text.size() && text[i] == '}') {
                i++;
                break;
            }
            std::string k, v;
            if (!quoted(k)) {
                error = "entity lump: bad key at " + std::to_string(i);
                return false;
            }
            skipWs();
            if (!quoted(v)) {
                error = "entity lump: bad value at " + std::to_string(i);
                return false;
            }
            e.kv.push_back({k, v});
        }
        out.push_back(std::move(e));
    }
}

// Spec Ops zones such as so_intel_boneyard.ff carry no world geometry: only a standalone
// "maps/<mission>.mapents" asset (the mission's whole entity lump, with its name inline) that the
// game layers over the base map's bsp. Import the base map's geometry, then use these entities.
static bool importOverlay(const std::string& ffPath, const std::vector<uint8_t>& z, size_t meAt, Document& doc,
                          ImportStats& st, std::string& error) {
    // The entity string ends at the NUL after the worldspawn lump and is numEntityChars (@16) long,
    // NUL included. Don't assume the struct size: these standalone MapEnts are 0x48 bytes, 16 less
    // than the 0x58 ones nested in a clipMap. The inline name is the string right before the lump.
    static const char kWorld[] = "\"classname\" \"worldspawn\"";
    auto ws = std::search(z.begin() + static_cast<std::ptrdiff_t>(meAt), z.end(), kWorld, kWorld + sizeof(kWorld) - 1);
    const void* lumpNul = ws == z.end() ? nullptr : std::memchr(&*ws, 0, static_cast<size_t>(z.end() - ws));
    uint32_t chars = rd32(z, meAt + 16);
    size_t lumpEnd = lumpNul ? static_cast<size_t>(static_cast<const uint8_t*>(lumpNul) - z.data()) : 0;
    if (!lumpNul || chars == 0 || chars > lumpEnd + 1 || lumpEnd + 1 - chars <= meAt) {
        error = "MapEnts entity string doesn't match its length field";
        return false;
    }
    size_t textAt = lumpEnd + 1 - chars;
    size_t nameAt = textAt - 1;  // the name's NUL
    while (nameAt > meAt && z[nameAt - 1] >= 0x20 && z[nameAt - 1] < 0x7f) nameAt--;
    std::string meName(reinterpret_cast<const char*>(z.data() + nameAt), textAt - 1 - nameAt);
    std::vector<Entity> ents;
    if (!parseEntityString(std::string(reinterpret_cast<const char*>(z.data() + textAt), chars - 1), ents, error))
        return false;

    // The base map: the longest suffix after an '_' of this file's name that is a zone beside it
    // (so_intel_boneyard -> boneyard, so_crossing_so_bridge -> so_bridge).
    namespace fs = std::filesystem;
    const fs::path path(ffPath);
    const std::string stem = path.stem().string();
    std::string base;
    for (size_t at = stem.find('_'); at != std::string::npos; at = stem.find('_', at + 1)) {
        std::string cand = stem.substr(at + 1);
        if (!cand.empty() && fs::exists(path.parent_path() / (cand + ".ff"))) {
            base = cand;
            break;
        }
    }
    Document baseDoc;
    ImportStats baseSt;
    std::string baseErr = base.empty() ? "no base map zone found next to it" : "";
    bool haveBase = !base.empty() &&
                    importFastfile((path.parent_path() / (base + ".ff")).string(), baseDoc, baseSt, baseErr);

    // The overlay is additive: a mission lump holds only what the mission adds (so_ac130_co_hunted
    // has 6 entities, no lights/nodes/spawners/player start), so the result is the base map's
    // entities plus these, with the overlay's worldspawn keys (sun, ambient...) winning.
    st = haveBase ? baseSt : ImportStats{};
    st.partial = true;
    doc.reset();
    if (haveBase) {
        doc.ents = std::move(baseDoc.ents);  // worldspawn (with the brushes) first
        doc.refMesh = baseDoc.refMesh;
    } else {
        doc.ents.clear();
    }
    if (doc.ents.empty() || doc.ents[0].classname() != "worldspawn") {
        Entity world;
        world.set("classname", "worldspawn");
        doc.ents.insert(doc.ents.begin(), std::move(world));
    }
    size_t added = 0;
    for (Entity& e : ents) {
        if (e.classname() == "worldspawn") {
            for (const KeyValue& kv : e.kv) doc.ents[0].set(kv.key, kv.value);
            continue;
        }
        doc.ents.push_back(std::move(e));
        added++;
    }
    st.entities = doc.ents.size();
    st.notes = "Spec Ops overlay (" + meName + "): " +
               (haveBase ? base + ".ff geometry + entities" : "no geometry (" + baseErr + ")") + ", plus " +
               std::to_string(added) + " mission entities";
    if (haveBase && !baseSt.notes.empty()) st.notes += "; base: " + baseSt.notes;
    doc.dirty = true;
    doc.touch();
    error.clear();
    return true;
}

bool importFastfile(const std::string& ffPath, Document& doc, ImportStats& st, std::string& error) {
    st = ImportStats{};
    std::vector<uint8_t> file;
    {
        std::ifstream f(ffPath, std::ios::binary | std::ios::ate);
        if (!f) {
            error = "can't read " + ffPath;
            return false;
        }
        file.resize(static_cast<size_t>(f.tellg()));
        f.seekg(0);
        f.read(reinterpret_cast<char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }
    FastfileKind kind = classifyFastfile(file.data(), file.size());
    if (kind != FastfileKind::Unsigned && kind != FastfileKind::Signed) {
        error = "not an IW fastfile";
        return false;
    }
    Fastfile ff;
    if (!readFastfile(file, ff, error)) {
        if (error.empty()) error = "couldn't read the fastfile";
        return false;
    }
    const std::vector<uint8_t>& z = ff.zone;

    // Zones the editor compiled: the whole zone loads through the simulator.
    {
        zone::SimulatedZone sim;
        std::string simErr;
        if (zone::simulateLoad(z, sim, simErr)) {
            auto* g = static_cast<const iw4::GfxWorld*>(sim.find(kGfxWorld));
            auto* c = static_cast<const iw4::clipMap_t*>(sim.find(kClipMap));
            if (!c) c = static_cast<const iw4::clipMap_t*>(sim.find(14));
            auto* me = static_cast<const iw4::MapEnts*>(sim.find(kMapEnts));
            if (!c && !g && !me) {
                error = "this zone has no map in it (no clipMap, GfxWorld or MapEnts)";
                return false;
            }
            std::shared_ptr<RefMesh> mesh = g ? convertGfxWorld(*g, st) : nullptr;
            fillDocument(doc, me, c, mesh, st, error);
            st.notes = "whole zone loaded";
            error.clear();
            return true;
        }
    }

    // Stock zones: find the world assets by structure, then load them on their own.
    st.partial = true;
    size_t meAt = 0;
    if (!findMapEnts(z, meAt)) {
        error = "no map entity lump (worldspawn) in this zone: it isn't a map zone";
        return false;
    }
    const uint64_t nameRef = rd64(z, meAt);
    if (nameRef == kFollow)  // a standalone MapEnts with its own name: a Spec Ops overlay zone
        return importOverlay(ffPath, z, meAt, doc, st, error);
    if (!isVirtualRef(nameRef)) {
        error = "unexpected MapEnts layout (name isn't a back-reference)";
        return false;
    }
    std::vector<size_t> sameName = findQword(z, nameRef);
    // clipMap: same name, inline MapEnts; confirmed by walking it onto the MapEnts we found.
    size_t clipAt = 0;
    std::vector<uint64_t> sideRefs;
    for (size_t o : sameName) {
        uint64_t me = rd64(z, o + 304);
        if (rd32(z, o + 8) > 1 || (me != kFollow && me != kInsert) || rd32(z, o + 12) == 0) continue;
        zone::SimulatedZone sim;
        std::vector<zone::PartialResult> res;
        std::string e;
        zone::PartialAsset pa;
        pa.type = kClipMap;
        pa.fileOffset = o;
        if (zone::simulatePartial(z, {pa}, sim, res, e) && res[0].mapEntsOffset == meAt) {
            clipAt = o;
            sideRefs = res[0].rawRefs;
            break;
        }
    }
    if (!clipAt) {
        error = "found the entity lump but not a clipMap that loads it";
        return false;
    }
    const uint32_t clipPlanes = rd32(z, clipAt + 12);
    const uint64_t clipPlanesRef = rd64(z, clipAt + 16);
    size_t gfxAt = 0;
    for (size_t o : sameName) {
        if (o == clipAt) continue;
        uint64_t base = rd64(z, o + 8);
        if (rd32(z, o + 16) == clipPlanes && rd32(z, o + 20) && rd32(z, o + 24) && rd32(z, o + 64) &&
            rd32(z, o + 64) < 65536 && (base == kFollow || isVirtualRef(base))) {
            gfxAt = o;
            break;
        }
    }

    // One run: GfxWorld first (anchored at its planes, which clipMap's planes reference), then
    // clipMap (anchored at its brushsides, the lowest address its brushes reference).
    std::vector<zone::PartialAsset> plan;
    if (gfxAt && isVirtualRef(clipPlanesRef)) {
        zone::PartialAsset g;
        g.type = kGfxWorld;
        g.fileOffset = gfxAt;
        g.hasAnchor = true;
        g.anchor = refOffset(clipPlanesRef);
        plan.push_back(g);
    }
    zone::PartialAsset c;
    c.type = kClipMap;
    c.fileOffset = clipAt;
    uint32_t lowest = UINT32_MAX;
    for (uint64_t r : sideRefs)
        if (isVirtualRef(r)) lowest = std::min(lowest, refOffset(r));
    if (lowest != UINT32_MAX) {
        c.hasAnchor = true;
        c.anchor = lowest;
    }
    plan.push_back(c);

    zone::SimulatedZone sim;
    std::vector<zone::PartialResult> res;
    std::string e;
    bool ok = zone::simulatePartial(z, plan, sim, res, e);
    if (!ok && plan.size() == 2) {  // render world failed: keep collision + entities
        st.notes += "render geometry skipped (" + e + "); ";
        plan.erase(plan.begin());
        res.clear();
        ok = zone::simulatePartial(z, plan, sim, res, e);
    }
    if (!ok) {
        error = "loading the map's clipMap failed: " + e;
        return false;
    }
    auto* cm = static_cast<const iw4::clipMap_t*>(sim.find(kClipMap));
    auto* me = static_cast<const iw4::MapEnts*>(sim.find(kMapEnts));
    auto* g = static_cast<const iw4::GfxWorld*>(sim.find(kGfxWorld));
    for (const zone::PartialResult& r : res)
        if (!r.stopped.empty()) st.notes += "stopped at " + r.stopped + " (not needed for geometry); ";
    if (!g) st.notes += gfxAt ? "" : "no GfxWorld found; ";
    if (g && std::getenv("IW4R_DUMP_DPVS")) {  // debug: how a stock map orders/ranges its surfaces
        const iw4::GfxWorldDpvsStatic& v = g->dpvs;
        std::printf("dpvs: surfaces %u static %u noDecal %u litOpaque [%u,%u) litTrans [%u,%u) shadowCaster [%u,%u) "
                    "emissive [%u,%u) skies %d\n",
                    g->surfaceCount, v.staticSurfaceCount, v.staticSurfaceCountNoDecal, v.litOpaqueSurfsBegin,
                    v.litOpaqueSurfsEnd, v.litTransSurfsBegin, v.litTransSurfsEnd, v.shadowCasterSurfsBegin,
                    v.shadowCasterSurfsEnd, v.emissiveSurfsBegin, v.emissiveSurfsEnd, g->skyCount);
        std::string last;
        for (unsigned i = 0; i < g->surfaceCount; i++) {
            const iw4::GfxSurface& s = v.surfaces[i];
            const char* mn = s.material && s.material->info.name ? s.material->info.name : "?";
            std::string tag = std::string(mn) + " lmap " + std::to_string(s.laf.fields.lightmapIndex) + " sortKey " +
                              std::to_string(s.material ? s.material->info.sortKey : -1);
            if (tag != last) std::printf("  sorted %5u: %s\n", i, tag.c_str());
            last = tag;
        }
    }
    if (g && std::getenv("IW4R_DUMP_LIGHTGRID")) {  // debug: average of each of the 56 light grid colours
        const iw4::GfxLightGrid& lg = g->lightGrid;
        std::printf("lightgrid: entries %u colors %u mins %u %u %u maxs %u %u %u\n", lg.entryCount, lg.colorCount, lg.mins[0], lg.mins[1],
                    lg.mins[2], lg.maxs[0], lg.maxs[1], lg.maxs[2]);
        std::printf("primary lights: %u, lastSun %u\n", g->primaryLightCount, g->lastSunPrimaryLightIndex);
        std::map<int, int> byLight;
        for (unsigned i = 0; i < g->surfaceCount; i++) byLight[g->dpvs.surfaces[i].laf.fields.primaryLightIndex]++;
        for (auto [l, n] : byLight) std::printf("  surfaces lit by primary %d: %d\n", l, n);
        for (unsigned i = 0; i < g->primaryLightCount && i < 64; i++) {
            const unsigned sc = g->shadowGeom ? g->shadowGeom[i].surfaceCount : 0, mc = g->shadowGeom ? g->shadowGeom[i].smodelCount : 0;
            const unsigned hulls = g->lightRegion ? g->lightRegion[i].hullCount : 0;
            unsigned axes = 0;
            if (g->lightRegion)
                for (unsigned h = 0; h < hulls; h++) axes += g->lightRegion[i].hulls[h].axisCount;
            std::printf("  light %2u: shadow surfs %u smodels %u, region hulls %u axes %u", i, sc, mc, hulls, axes);
            if (hulls && g->lightRegion) {
                const iw4::GfxLightRegionHull& h = g->lightRegion[i].hulls[0];
                std::printf("\n    kdop mid");
                for (int k = 0; k < 9; k++) std::printf(" %.1f", h.kdopMidPoint[k]);
                std::printf("\n    kdop half");
                for (int k = 0; k < 9; k++) std::printf(" %.1f", h.kdopHalfSize[k]);
                for (unsigned k = 0; k < h.axisCount; k++)
                    std::printf("\n    axis %.3f %.3f %.3f mid %.1f half %.1f", h.axis[k].dir[0], h.axis[k].dir[1], h.axis[k].dir[2],
                                h.axis[k].midPoint, h.axis[k].halfSize);
            }
            std::printf("\n");
        }
        std::printf("lightmaps: %d\n", g->draw.lightmapCount);
        for (int i = 0; i < g->draw.lightmapCount && g->draw.lightmaps; i++) {
            const iw4::GfxImage* p = g->draw.lightmaps[i].primary;
            const iw4::GfxImage* s = g->draw.lightmaps[i].secondary;
            std::printf("  %d: primary %ux%u fmt %x, secondary %ux%u fmt %x\n", i, p ? p->width : 0, p ? p->height : 0,
                        p && p->texture.loadDef ? p->texture.loadDef->format : 0, s ? s->width : 0, s ? s->height : 0,
                        s && s->texture.loadDef ? s->texture.loadDef->format : 0);
        }
        std::map<int, int> byLmap;
        for (unsigned i = 0; i < g->surfaceCount; i++) byLmap[g->dpvs.surfaces[i].laf.fields.lightmapIndex]++;
        for (auto [l, n] : byLmap) std::printf("  surfaces on lightmap %d: %d\n", l, n);
        std::printf("reflection probes: %u\n", g->draw.reflectionProbeCount);
        for (unsigned i = 0; i < g->draw.reflectionProbeCount && i < 4; i++) {
            const iw4::GfxImage* im = g->draw.reflectionProbes ? g->draw.reflectionProbes[i] : nullptr;
            if (!im) continue;
            const iw4::GfxImageLoadDef* ld = im->texture.loadDef;
            std::printf("  probe %u at %.0f %.0f %.0f: %s %ux%u map %d sem %d cat %d", i, g->draw.reflectionProbeOrigins[i].origin[0],
                        g->draw.reflectionProbeOrigins[i].origin[1], g->draw.reflectionProbeOrigins[i].origin[2], im->name ? im->name : "?",
                        im->width, im->height, im->mapType, static_cast<int>(im->semantic), im->category);
            if (ld) {
                std::printf(" levels %d flags %x format %x bytes %d", ld->levelCount, ld->flags, ld->format, ld->resourceSize);
                if (i == 1 && std::getenv("IW4R_DUMP_PROBE"))
                    std::ofstream(std::getenv("IW4R_DUMP_PROBE"), std::ios::binary)
                        .write(reinterpret_cast<const char*>(ld->data), ld->resourceSize);
                if (i == 0 && ld->resourceSize > 0) {  // average of face 0's first 64 texels (BGRA)
                    std::printf("\n    first texels:");
                    for (int t = 0; t < 8 && t * 4 + 3 < ld->resourceSize; t++)
                        std::printf(" %02x%02x%02x%02x", ld->data[t * 4 + 3], ld->data[t * 4 + 2], ld->data[t * 4 + 1], ld->data[t * 4]);
                }
            }
            std::printf("\n");
        }
        if (const char* zp = std::getenv("IW4R_DUMP_ZONE")) {
            std::ofstream(zp, std::ios::binary).write(reinterpret_cast<const char*>(z.data()), static_cast<std::streamsize>(z.size()));
        }
        if (lg.colors && lg.colorCount) {
            for (int i = 0; i < 56; i++) {
                double s[3] = {0, 0, 0};
                for (unsigned c = 0; c < lg.colorCount; c++)
                    for (int k = 0; k < 3; k++) s[k] += lg.colors[c].rgb[i][k];
                std::printf("  %2d: %6.1f %6.1f %6.1f\n", i, s[0] / lg.colorCount, s[1] / lg.colorCount, s[2] / lg.colorCount);
            }
        }
    }
    if (std::getenv("IW4R_DUMP_SMODELS")) dumpModels(g, cm, me);
    std::shared_ptr<RefMesh> mesh = g ? convertGfxWorld(*g, st) : nullptr;
    fillDocument(doc, me, cm, mesh, st, error);
    error.clear();
    return true;
}

}  // namespace mapimport
