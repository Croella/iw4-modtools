#include "zonebuild.h"
#include <cstddef>
#include <cstdint>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <miniz.h>
#include "fastfile.h"

#include "iw4.h"

using zone::ZoneWriter;

namespace {
// A pointer field holding the on-disk "follow" sentinel (-1): the pointee is written inline next.
template <class T>
T* follow() {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(zone::kPtrFollow));
}
// A pointer field holding an encoded back-reference (zone::offsetPointer).
template <class T>
T* ref(uint64_t encoded) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(encoded));
}
uint64_t virtualRef(uint32_t base, size_t index, size_t elemSize) {
    return zone::offsetPointer(zone::BLOCK_VIRTUAL, static_cast<uint32_t>(base + index * elemSize));
}

// Load_<Asset>Ptr, the wrapper every asset pointer goes through (top level or nested):
// push TEMP, align 4, reserve the insert slot for -2, load the struct + sub-data, pop.
// Returns a back-reference to the insert slot (0 for -1): later pointers to the same asset use
// it, and the loader resolves them through the slot (DB_ConvertOffsetToPointer on the slot).
uint64_t loadAssetPtr(ZoneWriter& w, uint64_t header, const std::function<void(ZoneWriter&)>& load) {
    w.push(zone::BLOCK_TEMP);
    w.align(zone::kAlign4);
    uint64_t slot = header == zone::kPtrInsert ? w.insertPointer() : 0;
    load(w);
    w.pop();
    return slot;
}

// A cplane_s array (align 4); returns its VIRTUAL position for back-references.
uint32_t writePlanes(ZoneWriter& w, const std::vector<clip::Plane>& planes) {
    uint32_t at = w.align(zone::kAlign4);
    for (const clip::Plane& p : planes) {
        iw4::cplane_s g{};
        std::memcpy(g.normal, p.normal, sizeof(g.normal));
        g.dist = p.dist;
        g.type = p.type;
        g.pad[0] = p.signbits;  // CoD4 keeps signbits here; harmless if unused
        w.writeStruct(g);
    }
    return at;
}

// Push/pop around a RUNTIME array: the loader zero-fills it, so it has no file bytes.
void runtimeArray(ZoneWriter& w, uint32_t align, size_t bytes) {
    w.push(zone::BLOCK_RUNTIME);
    w.align(align);
    w.zeros(bytes);
    w.pop();
}

// Load_GfxImagePtr (0x11a860) -> Load_GfxImage (0x11a7b0): struct 0x28 (TEMP); push VIRTUAL;
// name; texture = Load_GfxImageLoadDefPtr (0x11ad80): push TEMP, header 16 + resourceSize pixel
// bytes (the pixels only live in TEMP until the texture is created); pop; pop.
uint64_t writeImage(ZoneWriter& w, const gfx::Image& im) {
    return loadAssetPtr(w, zone::kPtrInsert, [&](ZoneWriter& ww) {
        iw4::GfxImage g{};
        g.texture.loadDef = reinterpret_cast<iw4::GfxImageLoadDef*>(static_cast<uintptr_t>(zone::kPtrInsert));
        g.mapType = static_cast<char>(im.mapType);
        g.semantic = static_cast<iw4::TextureSemantic>(im.semantic);
        g.category = static_cast<char>(im.category);
        g.cardMemory.platform[0] = g.cardMemory.platform[1] = static_cast<int>(im.pixels.size());
        g.width = im.width;
        g.height = im.height;
        g.depth = 1;
        g.delayLoadPixels = true;  // as in stock lightmaps/probes, which carry their pixels inline
        g.name = follow<const char>();
        ww.writeStruct(g);
        ww.push(zone::BLOCK_VIRTUAL);
        ww.xstring(im.name);
        loadAssetPtr(ww, zone::kPtrInsert, [&](ZoneWriter& w3) {
            uint8_t hdr[16] = {};
            hdr[0] = im.levelCount;
            uint32_t size = static_cast<uint32_t>(im.pixels.size());
            std::memcpy(hdr + 4, &im.loadFlags, 4);
            std::memcpy(hdr + 8, &im.format, 4);
            std::memcpy(hdr + 12, &size, 4);
            w3.write(hdr, sizeof(hdr));
            w3.write(im.pixels.data(), im.pixels.size());
        });
        ww.pop();
    });
}

void put64(std::vector<uint8_t>& b, size_t at, uint64_t v) { std::memcpy(b.data() + at, &v, 8); }

// Sky cubemap image: the stock header (cube, load-from-file category) under our image name, with
// a 16-byte load def of resourceSize 0, so the game streams images/<name>.iwi from the IWDs.
uint64_t writeSkyImage(ZoneWriter& w, const sky::Source& src, const std::string& name) {
    return loadAssetPtr(w, zone::kPtrInsert, [&](ZoneWriter& ww) {
        std::vector<uint8_t> im = src.image;
        put64(im, 0, zone::kPtrInsert);  // texture.loadDef: inline + slot (as stock)
        put64(im, 32, zone::kPtrFollow);  // name
        ww.write(im.data(), im.size());
        ww.push(zone::BLOCK_VIRTUAL);
        ww.xstring(name);
        loadAssetPtr(ww, zone::kPtrInsert, [&](ZoneWriter& w3) {
            std::vector<uint8_t> ld = src.imageLoadDef;
            std::memset(ld.data() + 12, 0, 4);  // resourceSize 0: pixels come from the .iwi
            w3.write(ld.data(), ld.size());
        });
        ww.pop();
    });
}

// The embedded sky material, written in full as Load_Material walks it (see zonesim materialPtr):
// struct; name; technique set inline (only slot 4 "sky": one pass, rebuilt "pp" vertex decl, the
// stock sky.hlsl shaders and args); texture table (colorMap -> the sky image, back-referenced to
// the slot GfxWorld.skies wrote); state bits.
uint64_t writeSkyMaterial(ZoneWriter& w, const sky::Source& src, uint64_t imageSlot, const std::string& name) {
    using namespace zone;
    return loadAssetPtr(w, kPtrInsert, [&](ZoneWriter& ww) {
        std::vector<uint8_t> m = src.material;
        put64(m, 0, kPtrFollow);   // info.name
        put64(m, 88, kPtrFollow);  // techniqueSet
        put64(m, 96, kPtrFollow);  // textureTable
        put64(m, 104, 0);          // constantTable
        put64(m, 112, src.stateBits.empty() ? 0 : kPtrFollow);
        m[80] = 1;
        m[81] = 0;
        m[82] = static_cast<uint8_t>(src.stateBits.size() / 8);
        ww.write(m.data(), m.size());
        ww.push(BLOCK_VIRTUAL);
        ww.xstring(name);
        loadAssetPtr(ww, kPtrFollow, [&](ZoneWriter& w2) {  // Load_MaterialTechniqueSetPtr
            std::vector<uint8_t> ts(0x198, 0);
            put64(ts, 0, kPtrFollow);
            put64(ts, 24 + 8 * 4, kPtrFollow);  // techniques[4]
            w2.write(ts.data(), ts.size());
            w2.push(BLOCK_VIRTUAL);
            w2.xstring("iw4r_sky");
            w2.align(kAlign4);
            std::vector<uint8_t> tech = src.technique;
            put64(tech, 0, kPtrFollow);
            tech[10] = 1;
            tech[11] = 0;
            w2.write(tech.data(), tech.size());
            std::vector<uint8_t> pass = src.pass;
            put64(pass, 0, kPtrFollow);   // vertexDecl
            put64(pass, 8, kPtrFollow);   // vertexShader
            put64(pass, 16, kPtrFollow);  // pixelShader
            put64(pass, 32, src.args.empty() ? 0 : kPtrFollow);
            w2.write(pass.data(), pass.size());
            loadAssetPtr(w2, kPtrFollow, [&](ZoneWriter& w3) {  // vertex decl "pp"
                std::vector<uint8_t> vd(0xB0, 0);
                put64(vd, 0, kPtrFollow);
                vd[8] = 1;  // one stream: source position (0) -> dest position (0)
                w3.write(vd.data(), vd.size());
                w3.push(BLOCK_VIRTUAL);
                w3.xstring("iw4r_sky_pp");
                w3.pop();
            });
            auto shader = [&](const sky::ShaderBlob& sh, const char* shName) {
                loadAssetPtr(w2, kPtrFollow, [&](ZoneWriter& w3) {
                    std::vector<uint8_t> h = sh.header;
                    put64(h, 0, kPtrFollow);
                    put64(h, 8, 0);  // D3D object (runtime)
                    put64(h, 16, sh.code.empty() ? 0 : kPtrFollow);
                    w3.write(h.data(), h.size());
                    w3.push(BLOCK_VIRTUAL);
                    w3.xstring(shName);
                    if (!sh.code.empty()) {
                        w3.align(kAlign4);
                        w3.write(sh.code.data(), sh.code.size());
                    }
                    w3.pop();
                });
            };
            shader(src.vs, "iw4r_sky_vs");
            shader(src.ps, "iw4r_sky_ps");
            if (!src.args.empty()) {
                std::vector<uint8_t> args = src.args;
                for (size_t a = 0; a < src.literals.size(); a++)
                    if (!src.literals[a].empty()) put64(args, 16 * a + 8, kPtrFollow);
                w2.align(kAlign4);
                w2.write(args.data(), args.size());
                for (const auto& lit : src.literals)
                    if (!lit.empty()) {
                        w2.align(kAlign4);
                        w2.write(lit.data(), lit.size());
                    }
            }
            w2.xstring("sky");  // technique name
            w2.pop();
        });
        ww.align(kAlign4);
        std::vector<uint8_t> td = src.textureDef;
        put64(td, 8, imageSlot);
        ww.write(td.data(), td.size());
        if (!src.stateBits.empty()) {
            ww.align(kAlign4);
            ww.write(src.stateBits.data(), src.stateBits.size());
        }
        ww.pop();
    });
}

// An image that streams from images/<name>.iwi: the stock header (category "load from file")
// and a 16-byte load def with resourceSize 0. Inline + insert slot, so later uses back-reference it.
uint64_t writeFileImage(ZoneWriter& w, const std::vector<uint8_t>& header, const std::vector<uint8_t>& loadDef,
                        const std::string& name) {
    return loadAssetPtr(w, zone::kPtrInsert, [&](ZoneWriter& ww) {
        std::vector<uint8_t> im = header;
        put64(im, 0, zone::kPtrInsert);   // texture.loadDef
        put64(im, 32, zone::kPtrFollow);  // name
        ww.write(im.data(), im.size());
        ww.push(zone::BLOCK_VIRTUAL);
        ww.xstring(name);
        loadAssetPtr(ww, zone::kPtrInsert, [&](ZoneWriter& w3) {
            std::vector<uint8_t> ld = loadDef.size() == 16 ? loadDef : std::vector<uint8_t>(16, 0);
            std::memset(ld.data() + 12, 0, 4);  // resourceSize 0: pixels come from the .iwi
            w3.write(ld.data(), ld.size());
        });
        ww.pop();
    });
}

// A stock world material written in full (Load_Material order, see zonesim materialPtr): struct;
// name; technique set as a by-name reference (",wc_l_sm_b0c0..." resolves to common.ff's);
// texture table (colorMap / normalMap / specularMap) with each image inline the first time and
// back-referenced after; constants; state bits.
uint64_t writeWorldMaterial(ZoneWriter& w, const matdef::Def& d, std::map<std::string, uint64_t>& imageSlots) {
    using namespace zone;
    const std::vector<const matdef::Texture*> tex = matdef::usedTextures(d);
    const std::string techset = matdef::techsetFor(d);
    return loadAssetPtr(w, kPtrInsert, [&](ZoneWriter& ww) {
        std::vector<uint8_t> m = d.header;
        put64(m, 0, kPtrFollow);
        put64(m, 88, kPtrFollow);
        put64(m, 96, tex.empty() ? 0 : kPtrFollow);
        put64(m, 104, d.constants.empty() ? 0 : kPtrFollow);
        put64(m, 112, d.stateBits.empty() ? 0 : kPtrFollow);
        m[80] = static_cast<uint8_t>(tex.size());
        m[81] = static_cast<uint8_t>(d.constants.size() / 32);
        m[82] = static_cast<uint8_t>(d.stateBits.size() / 8);
        ww.write(m.data(), m.size());
        ww.push(BLOCK_VIRTUAL);
        ww.xstring(d.name);
        loadAssetPtr(ww, kPtrFollow, [&](ZoneWriter& w2) {  // technique set by name
            std::vector<uint8_t> ts(0x198, 0);
            put64(ts, 0, kPtrFollow);
            w2.write(ts.data(), ts.size());
            w2.push(BLOCK_VIRTUAL);
            w2.xstring("," + techset);
            w2.pop();
        });
        if (!tex.empty()) {
            ww.align(kAlign4);
            std::vector<bool> inlineImage(tex.size());
            for (size_t i = 0; i < tex.size(); i++) {
                std::vector<uint8_t> def = tex[i]->def;
                auto it = imageSlots.find(tex[i]->image);
                inlineImage[i] = it == imageSlots.end();
                put64(def, 8, inlineImage[i] ? kPtrInsert : it->second);
                ww.write(def.data(), def.size());
            }
            for (size_t i = 0; i < tex.size(); i++)
                if (inlineImage[i] && !imageSlots.count(tex[i]->image))
                    imageSlots[tex[i]->image] = writeFileImage(ww, tex[i]->imageHeader, tex[i]->loadDef, tex[i]->image);
        }
        if (!d.constants.empty()) {
            ww.align(kAlign16);
            ww.write(d.constants.data(), d.constants.size());
        }
        if (!d.stateBits.empty()) {
            ww.align(kAlign4);
            ww.write(d.stateBits.data(), d.stateBits.size());
        }
        ww.pop();
    });
}

// The editor (like .map files) names world materials without their "wc/" asset prefix.
std::string worldMaterialAssetName(const std::string& n) { return n.find('/') == std::string::npos ? "wc/" + n : n; }

// Load_MaterialHandle (0x11d120) -> Load_Material (0x11cf00) for a by-name reference: the name
// starts with ',' so DB_AddXAsset (0x12e700) resolves it to the already-loaded material (or a
// default one) instead of adding it. Only MaterialInfo.name is set; every other pointer is null,
// which Load_Material checks before loading.
uint64_t writeMaterialRef(ZoneWriter& w, const std::string& name) {
    return loadAssetPtr(w, zone::kPtrInsert, [&](ZoneWriter& ww) {
        iw4::Material m{};
        m.info.name = follow<const char>();
        ww.writeStruct(m);
        ww.push(zone::BLOCK_VIRTUAL);
        ww.xstring("," + name);
        ww.pop();
    });
}

// Load_MapEnts (0x11cb80, SP 0xe0590 identical): struct 0x58; push VIRTUAL; name (XString);
// entityString (byte-aligned, numEntityChars bytes); MapTriggers inline (all null here); stages
// (align 4, 24 bytes each, then each stage's name as an XString); pop.
// withDefaultStage: one stage 0 (whole map, sun = primary light 1). SP needs it: its per-frame
// stage lookup (iw4sp+0x46c20) returns 0 when the player is in no stage trigger and the caller
// (iw4sp+0x85f8) then reads stages[0] with no null check. MP zones keep stages null.
void writeMapEnts(ZoneWriter& w, const std::string& name, const std::string& entityString,
                  bool withDefaultStage = false) {
    iw4::MapEnts m{};
    m.name = follow<const char>();
    m.entityString = follow<char>();
    m.numEntityChars = static_cast<int>(entityString.size() + 1);  // incl. NUL
    if (withDefaultStage) {
        m.stages = follow<iw4::Stage>();
        m.stageCount = 1;
    }
    w.writeStruct(m);
    w.push(zone::BLOCK_VIRTUAL);
    w.xstring(name);
    w.align(zone::kAlign1);
    w.write(entityString.c_str(), entityString.size() + 1);
    if (withDefaultStage) {
        w.align(zone::kAlign4);
        iw4::Stage st{};
        st.name = follow<const char>();
        st.sunPrimaryLightIndex = 1;  // ComWorld/GfxWorld primary light 1 is the sun
        w.writeStruct(st);
        w.xstring("default");
    }
    w.pop();
}
static_assert(sizeof(iw4::Stage) == 24 && offsetof(iw4::MapEnts, stages) == 0x48 &&
                  offsetof(iw4::MapEnts, stageCount) == 0x50,
              "Stage/MapEnts layout differs from Load_MapEnts");

iw4::Bounds toGame(const clip::Bounds& b) {
    iw4::Bounds r{};
    std::memcpy(r.midPoint, b.midPoint, sizeof(r.midPoint));
    std::memcpy(r.halfSize, b.halfSize, sizeof(r.halfSize));
    return r;
}

iw4::cLeaf_t toGame(const clip::Leaf& l) {
    iw4::cLeaf_t r{};
    r.firstCollAabbIndex = l.firstCollAabbIndex;
    r.collAabbCount = l.collAabbCount;
    r.brushContents = static_cast<int>(l.brushContents);
    r.terrainContents = static_cast<int>(l.terrainContents);
    r.bounds = toGame(l.bounds);
    r.leafBrushNode = static_cast<int>(l.leafBrushNode);
    return r;
}
}  // namespace

std::string documentToEntityString(const Document& doc) {
    std::string s;
    for (const Entity& e : doc.ents) {
        if (e.kv.empty()) continue;
        s += "{\n";
        for (const KeyValue& kv : e.kv) s += "\"" + kv.key + "\" \"" + kv.value + "\"\n";
        s += "}\n";
    }
    return s;
}

ZoneAsset buildMapEnts(const std::string& name, const std::string& entityString) {
    return {IW4_MAPENTS, [name, entityString](ZoneWriter& w) { writeMapEnts(w, name, entityString); }};
}

// Load_clipMap_t (0x1242b0). Field order below is the loader's, which is NOT struct order
// (leafbrushes before leafbrushNodes, smodelNodes before mapEnts). Element pointers inside the
// arrays are back-references into the arrays already written (sides -> planes, nodes -> planes,
// brushes -> brushsides/brushEdges, leaf-brush nodes -> leafbrushes); a -1 there would load a
// single element inline instead. Every array is under push(VIRTUAL) except the dynEnt
// pose/client/coll lists (RUNTIME, none emitted). Validated against stock boneyard: walking its
// clipMap in this order lands exactly on its inline MapEnts.
ZoneAsset buildClipMap(const std::string& name, const clip::ClipMapData& cm,
                       const std::string& entityString, std::shared_ptr<SharedWorld> shared,
                       bool withDefaultStage) {
    return {IW4_CLIPMAP_MP, [name, cm, entityString, shared, withDefaultStage](ZoneWriter& w) {
                using namespace zone;
                const size_t nb = cm.brushes.size();
                // Stock clipMaps back-reference GfxWorld's plane array instead of their own copy.
                const bool sharedPlanes = shared && shared->havePlanes && !cm.planes.empty();
                iw4::clipMap_t c{};
                c.name = follow<const char>();
                c.planeCount = static_cast<unsigned>(cm.planes.size());
                c.planes = cm.planes.empty() ? nullptr
                           : sharedPlanes    ? ref<iw4::cplane_s>(virtualRef(shared->planesAt, 0, 20))
                                             : follow<iw4::cplane_s>();
                c.numMaterials = static_cast<unsigned>(cm.materials.size());
                c.materials = cm.materials.empty() ? nullptr : follow<iw4::ClipMaterial>();
                c.numBrushSides = static_cast<unsigned>(cm.sides.size());
                c.brushsides = cm.sides.empty() ? nullptr : follow<iw4::cbrushside_t>();
                c.numBrushEdges = static_cast<unsigned>(cm.brushEdges.size());
                c.brushEdges = cm.brushEdges.empty() ? nullptr : follow<unsigned char>();
                c.numNodes = static_cast<unsigned>(cm.nodes.size());
                c.nodes = cm.nodes.empty() ? nullptr : follow<iw4::cNode_t>();
                c.numLeafs = static_cast<unsigned>(cm.leafs.size());
                c.leafs = cm.leafs.empty() ? nullptr : follow<iw4::cLeaf_t>();
                c.leafbrushNodesCount = static_cast<unsigned>(cm.leafBrushNodes.size());
                c.leafbrushNodes = cm.leafBrushNodes.empty() ? nullptr : follow<iw4::cLeafBrushNode_s>();
                c.numLeafBrushes = static_cast<unsigned>(cm.leafBrushes.size());
                c.leafbrushes = cm.leafBrushes.empty() ? nullptr : follow<unsigned short>();
                c.numSubModels = static_cast<unsigned>(cm.cmodels.size());
                c.cmodels = cm.cmodels.empty() ? nullptr : follow<iw4::cmodel_t>();
                c.numBrushes = static_cast<unsigned short>(nb);
                c.brushes = nb ? follow<iw4::cbrush_t>() : nullptr;
                c.brushBounds = nb ? follow<iw4::Bounds>() : nullptr;
                c.brushContents = nb ? follow<int>() : nullptr;
                c.mapEnts = ref<iw4::MapEnts>(kPtrInsert);  // inline + added as its own asset
                // One empty static-model AABB node, so a walk from node 0 finds nothing to test.
                c.smodelNodeCount = 1;
                c.smodelNodes = follow<iw4::SModelAabbNode>();
                c.checksum = cm.checksum;
                w.writeStruct(c);

                w.push(BLOCK_VIRTUAL);
                w.xstring(name);

                uint32_t planesAt = sharedPlanes ? shared->planesAt : 0;
                if (!cm.planes.empty() && !sharedPlanes) planesAt = writePlanes(w, cm.planes);
                if (!cm.materials.empty()) {
                    w.align(kAlign4);
                    for (const clip::Material& m : cm.materials) {
                        iw4::ClipMaterial g{};
                        g.name = follow<const char>();
                        g.surfaceFlags = static_cast<int>(m.surfaceFlags);
                        g.contents = static_cast<int>(m.contents);
                        w.writeStruct(g);
                    }
                    for (const clip::Material& m : cm.materials) w.xstring(m.name);
                }
                uint32_t sidesAt = 0;
                if (!cm.sides.empty()) {
                    sidesAt = w.align(kAlign4);
                    for (const clip::Side& s : cm.sides) {
                        iw4::cbrushside_t g{};
                        g.plane = ref<iw4::cplane_s>(virtualRef(planesAt, s.plane, sizeof(iw4::cplane_s)));
                        g.materialNum = s.material;
                        g.firstAdjacentSideOffset = static_cast<char>(s.firstAdjacentSideOffset);
                        g.edgeCount = static_cast<char>(s.edgeCount);
                        w.writeStruct(g);
                    }
                }
                uint32_t edgesAt = 0;
                if (!cm.brushEdges.empty()) {
                    edgesAt = w.align(kAlign1);
                    w.write(cm.brushEdges.data(), cm.brushEdges.size());
                }
                if (!cm.nodes.empty()) {
                    w.align(kAlign4);
                    for (const clip::Node& n : cm.nodes) {
                        iw4::cNode_t g{};
                        g.plane = ref<iw4::cplane_s>(virtualRef(planesAt, n.plane, sizeof(iw4::cplane_s)));
                        g.children[0] = n.children[0];
                        g.children[1] = n.children[1];
                        w.writeStruct(g);
                    }
                }
                if (!cm.leafs.empty()) {
                    w.align(kAlign4);
                    for (const clip::Leaf& l : cm.leafs) w.writeStruct(toGame(l));
                }
                uint32_t leafBrushesAt = 0;
                if (!cm.leafBrushes.empty()) {
                    leafBrushesAt = w.align(kAlign2);
                    w.write(cm.leafBrushes.data(), cm.leafBrushes.size() * sizeof(uint16_t));
                }
                if (!cm.leafBrushNodes.empty()) {
                    w.align(kAlign4);
                    for (const clip::LeafBrushNode& n : cm.leafBrushNodes) {
                        iw4::cLeafBrushNode_s g{};
                        g.axis = n.axis;
                        g.leafBrushCount = n.leafBrushCount;
                        g.contents = static_cast<int>(n.contents);
                        if (n.leafBrushCount > 0) {
                            g.data.leaf.brushes = ref<unsigned short>(
                                virtualRef(leafBrushesAt, n.firstLeafBrush, sizeof(uint16_t)));
                        } else {  // interior split node
                            g.data.children.dist = n.dist;
                            g.data.children.range = n.range;
                            g.data.children.childOffset[0] = n.childOffset[0];
                            g.data.children.childOffset[1] = n.childOffset[1];
                        }
                        w.writeStruct(g);
                    }
                }
                // leafsurfaces, verts, triIndices, triEdgeIsWalkable, borders, partitions,
                // aabbTrees: none until patches/terrain are compiled.
                if (!cm.cmodels.empty()) {
                    w.align(kAlign4);
                    for (const clip::CModel& m : cm.cmodels) {
                        iw4::cmodel_t g{};
                        g.bounds = toGame(m.bounds);
                        g.radius = m.radius;
                        g.leaf = toGame(m.leaf);
                        w.writeStruct(g);
                    }
                }
                if (nb) {
                    w.align(127);  // brushes and brushBounds are 128-byte aligned
                    for (const clip::Brush& b : cm.brushes) {
                        iw4::cbrush_t g{};
                        g.numsides = b.numSides;
                        if (b.numSides)
                            g.sides = ref<iw4::cbrushside_t>(virtualRef(sidesAt, b.firstSide, sizeof(iw4::cbrushside_t)));
                        if (b.edgeBytes) g.baseAdjacentSide = ref<unsigned char>(virtualRef(edgesAt, b.firstEdge, 1));
                        std::memcpy(g.axialMaterialNum, b.axialMaterial, sizeof(g.axialMaterialNum));
                        std::memcpy(g.firstAdjacentSideOffsets, b.firstAdjacentSideOffsets,
                                    sizeof(g.firstAdjacentSideOffsets));
                        std::memcpy(g.edgeCount, b.edgeCount, sizeof(g.edgeCount));
                        w.writeStruct(g);
                    }
                    w.align(127);
                    for (const clip::Brush& b : cm.brushes) w.writeStruct(toGame(b.bounds));
                    w.align(kAlign4);
                    for (const clip::Brush& b : cm.brushes) w.i32(static_cast<int>(b.contents));
                }
                w.align(kAlign4);
                w.writeStruct(iw4::SModelAabbNode{});
                // Load_MapEntsPtr (0x11cd50), nested: the entity lump becomes its own asset.
                loadAssetPtr(w, kPtrInsert, [&](ZoneWriter& ww) { writeMapEnts(ww, name, entityString, withDefaultStage); });
                // dynEnt def/pose/client/coll lists: none.
                w.pop();
            }};
}

// Load_ComWorld (0x1188a0): struct 0x18; push VIRTUAL; name; primaryLights (null); pop.
ZoneAsset buildComWorld(const std::string& name, bool withSun, const light::Environment* env) {
    // Sun primary light: from the map's worldspawn (via the bake) or a fixed default.
    Vec3 sunDir = normalize(Vec3{-0.4, 0.3, 0.86}), sunColor{1, 1, 1};
    if (env && env->hasSun) {
        sunDir = env->sunDir;
        sunColor = env->sunColor;
    }
    return {IW4_COMWORLD, [name, withSun, sunDir, sunColor](ZoneWriter& w) {
                iw4::ComWorld c{};
                c.name = follow<const char>();
                c.isInUse = 1;
                // Primary light 0 is the empty slot; light 1 is the sun (lastSunPrimaryLightIndex
                // 1 in GfxWorld). The count must match GfxWorld.primaryLightCount.
                c.primaryLightCount = withSun ? 2 : 0;
                c.primaryLights = withSun ? follow<iw4::ComPrimaryLight>() : nullptr;
                w.writeStruct(c);
                w.push(zone::BLOCK_VIRTUAL);
                w.xstring(name);
                if (withSun) {
                    w.align(zone::kAlign4);
                    iw4::ComPrimaryLight none{}, sun{};
                    sun.type = 1;  // GFX_LIGHT_TYPE_DIR
                    for (int k = 0; k < 3; k++) {
                        sun.color[k] = static_cast<float>(sunColor[k]);  // linear
                        sun.dir[k] = static_cast<float>(sunDir[k]);      // towards the sun
                    }
                    w.writeStruct(none);
                    w.writeStruct(sun);  // defName (+64) null in both
                }
                w.pop();
            }};
}

// Load_GfxWorld (0x11ae60), in its exact order; see gfxworld.h for the data conventions. All
// world arrays are VIRTUAL; the listed RUNTIME arrays are zero-filled by the loader (pointer -1,
// no file bytes). Materials are loaded once each as ','-name references through materialMemory
// (which loads before dpvs), and every surface back-references the material's insert slot.
// GfxWorld owns the plane array; clipMap back-references it (stock layout).
ZoneAsset buildGfxWorld(const std::string& name, const std::string& baseName, const gfx::GfxWorldData& gw,
                        const clip::ClipMapData& cm, std::shared_ptr<SharedWorld> shared,
                        std::shared_ptr<const sky::Source> skySrc, std::shared_ptr<const matdef::Library> materials) {
    return {IW4_GFXWORLD, [name, baseName, gw, planes = cm.planes, checksum = cm.checksum, shared, skySrc,
                           materials](ZoneWriter& w) {
                using namespace zone;
                const auto rt = follow<unsigned int>();  // RUNTIME arrays: allocated + zeroed
                const uint32_t ns = static_cast<uint32_t>(gw.surfaces.size());
                const uint32_t cellCount = 1, primaryLights = 2;
                const uint32_t visCount = (ns + 63) >> 5;
                iw4::GfxWorld g{};
                g.name = follow<const char>();
                g.baseName = follow<const char>();
                g.planeCount = static_cast<int>(planes.size());
                g.nodeCount = 1;
                g.surfaceCount = ns;
                const bool hasSky = skySrc && gw.skyMaterial >= 0 && !gw.skySurfaces.empty();
                g.skyCount = hasSky ? 1 : 0;
                g.skies = hasSky ? follow<iw4::GfxSky>() : nullptr;
                g.lastSunPrimaryLightIndex = 1;
                g.primaryLightCount = primaryLights;
                g.sortKeyLitDecal = 6;  // stock values (boneyard)
                g.sortKeyEffectDecal = 39;
                g.sortKeyEffectAuto = 48;
                g.sortKeyDistortion = 43;
                g.dpvsPlanes.cellCount = static_cast<int>(cellCount);
                g.dpvsPlanes.planes = planes.empty() ? nullptr : follow<iw4::cplane_s>();
                g.dpvsPlanes.nodes = follow<unsigned short>();
                g.dpvsPlanes.sceneEntCellBits = rt;
                g.aabbTreeCounts = follow<iw4::GfxCellTreeCount>();
                g.aabbTrees = follow<iw4::GfxCellTree>();
                g.cells = follow<iw4::GfxCell>();
                iw4::GfxWorldDraw& d = g.draw;
                d.reflectionProbeCount = 1;
                d.reflectionProbes = follow<iw4::GfxImage*>();
                d.reflectionProbeOrigins = follow<iw4::GfxReflectionProbe>();
                d.reflectionProbeTextures = follow<iw4::GfxTexture>();
                d.lightmapCount = 1;
                d.lightmaps = follow<iw4::GfxLightmapArray>();
                d.lightmapPrimaryTextures = follow<iw4::GfxTexture>();
                d.lightmapSecondaryTextures = follow<iw4::GfxTexture>();
                d.vertexCount = static_cast<unsigned>(gw.vertices.size());
                d.vd.vertices = gw.vertices.empty() ? nullptr : follow<iw4::GfxWorldVertex>();
                d.vertexLayerDataSize = 4;  // stock: 4 bytes of layer data even when unused
                d.vld.data = follow<char>();
                d.indexCount = static_cast<unsigned>(gw.indices.size());
                d.indices = gw.indices.empty() ? nullptr : follow<unsigned short>();
                const gfx::LightGrid& lg = gw.lightGrid;
                g.lightGrid.hasLightRegions = false;
                g.lightGrid.lastSunPrimaryLightIndex = 1;
                std::memcpy(g.lightGrid.mins, lg.mins, sizeof(lg.mins));
                std::memcpy(g.lightGrid.maxs, lg.maxs, sizeof(lg.maxs));
                g.lightGrid.rowAxis = lg.rowAxis;
                g.lightGrid.colAxis = lg.colAxis;
                g.lightGrid.rowDataStart = follow<unsigned short>();
                g.lightGrid.rawRowDataSize = static_cast<unsigned>(lg.rawRowData.size());
                g.lightGrid.rawRowData = follow<char>();
                g.lightGrid.entryCount = lg.entryCount;
                g.lightGrid.entries = follow<iw4::GfxLightGridEntry>();
                g.lightGrid.colorCount = lg.colorCount;
                g.lightGrid.colors = follow<iw4::GfxLightGridColors>();
                g.modelCount = 1;
                g.models = follow<iw4::GfxBrushModel>();
                for (int k = 0; k < 3; k++) {
                    g.bounds.midPoint[k] = (gw.mins[k] + gw.maxs[k]) * 0.5f;
                    g.bounds.halfSize[k] = (gw.maxs[k] - gw.mins[k]) * 0.5f;
                }
                g.checksum = checksum;  // stock: GfxWorld and clipMap checksums match
                g.materialMemoryCount = static_cast<int>(gw.materials.size());
                g.materialMemory = gw.materials.empty() ? nullptr : follow<iw4::MaterialMemory>();
                g.cellCasterBits = rt;
                g.cellHasSunLitSurfsBits = rt;
                g.sceneDynModel = follow<iw4::GfxSceneDynModel>();
                g.sceneDynBrush = follow<iw4::GfxSceneDynBrush>();
                g.primaryLightEntityShadowVis = rt;
                g.primaryLightDynEntShadowVis[0] = g.primaryLightDynEntShadowVis[1] = rt;
                g.nonSunPrimaryLightForModelDynEnt = follow<char>();
                g.shadowGeom = follow<iw4::GfxShadowGeometry>();
                g.lightRegion = follow<iw4::GfxLightRegion>();
                iw4::GfxWorldDpvsStatic& v = g.dpvs;
                v.smodelCount = 0;
                v.staticSurfaceCount = v.staticSurfaceCountNoDecal = ns;
                v.litOpaqueSurfsBegin = 0;
                v.litOpaqueSurfsEnd = v.litTransSurfsBegin = v.litTransSurfsEnd = ns;
                v.shadowCasterSurfsBegin = v.shadowCasterSurfsEnd = ns;
                v.emissiveSurfsBegin = v.emissiveSurfsEnd = ns;
                v.smodelVisDataCount = (0 + 63) >> 5;
                v.surfaceVisDataCount = visCount;
                for (int k = 0; k < 3; k++) {
                    v.smodelVisData[k] = follow<unsigned char>();
                    v.surfaceVisData[k] = follow<unsigned char>();
                }
                v.sortedSurfIndex = ns ? follow<unsigned short>() : nullptr;
                v.surfaces = ns ? follow<iw4::GfxSurface>() : nullptr;
                v.surfacesBounds = ns ? follow<iw4::GfxSurfaceBounds>() : nullptr;
                v.surfaceMaterials = follow<iw4::GfxDrawSurf>();
                v.surfaceCastsSunShadow = rt;
                for (int k = 0; k < 2; k++) {
                    g.dpvsDyn.dynEntCellBits[k] = rt;
                    for (int j = 0; j < 3; j++) g.dpvsDyn.dynEntVisData[k][j] = follow<char>();
                }
                g.fogTypesAllowed = 2;  // stock value
                w.writeStruct(g);

                w.push(BLOCK_VIRTUAL);
                w.xstring(name);
                w.xstring(baseName);
                // skies (one): GfxSky, its surface list, then the cube image (inline + slot, which
                // the sky material's texture table back-references later).
                uint64_t skyImageSlot = 0;
                if (hasSky) {
                    w.align(kAlign4);
                    iw4::GfxSky s{};
                    s.skySurfCount = static_cast<int>(gw.skySurfaces.size());
                    s.skyStartSurfs = follow<int>();
                    s.skyImage = reinterpret_cast<iw4::GfxImage*>(static_cast<uintptr_t>(kPtrInsert));
                    s.skySamplerState = static_cast<char>(skySrc->textureDef[6]);  // the sky texture's sampler
                    w.writeStruct(s);
                    w.align(kAlign4);
                    for (uint32_t i : gw.skySurfaces) w.u32(i);
                    skyImageSlot = writeSkyImage(w, *skySrc, gw.skyImage);
                }
                // dpvsPlanes (0x11ba20): planes, nodes, RUNTIME sceneEntCellBits.
                if (!planes.empty()) {
                    uint32_t at = writePlanes(w, planes);
                    if (shared) {
                        shared->planesAt = at;
                        shared->havePlanes = true;
                    }
                }
                w.align(kAlign2);
                w.write(std::array<uint16_t, 1>{1}.data(), 2);  // one leaf: cell 0
                runtimeArray(w, kAlign4, cellCount << 11);
                // aabbTreeCounts, then per cell its tree array (one leaf tree with every surface).
                w.align(kAlign4);
                w.u32(1);
                w.align(127);
                w.u64(kPtrFollow);  // GfxCellTree.aabbTree
                w.align(kAlign4);
                {
                    iw4::GfxAabbTree t{};
                    for (int k = 0; k < 3; k++) {
                        t.bounds.midPoint[k] = g.bounds.midPoint[k];
                        t.bounds.halfSize[k] = g.bounds.halfSize[k];
                    }
                    t.surfaceCount = t.surfaceCountNoDecal = static_cast<unsigned short>(ns);
                    t.startSurfIndex = 0;
                    t.startSurfIndexNoDecal = static_cast<unsigned short>(ns);  // second half
                    w.writeStruct(t);
                }
                // cells: one, no portals, sees reflection probe 0.
                w.align(kAlign4);
                {
                    iw4::GfxCell c{};
                    c.bounds = g.bounds;
                    c.reflectionProbeCount = 1;
                    c.reflectionProbes = follow<char>();
                    w.writeStruct(c);
                    w.align(kAlign1);
                    w.u8(0);
                }
                // draw (0x11bfd0)
                w.align(kAlign4);
                w.u64(kPtrInsert);  // reflectionProbes[0] -> image follows
                writeImage(w, gw.reflectionProbe);
                w.align(kAlign4);
                for (int k = 0; k < 3; k++) w.write(&g.bounds.midPoint[k], 4);  // probe origin
                runtimeArray(w, kAlign4, 8 * 1);                                  // probe textures
                w.align(kAlign4);
                w.u64(kPtrInsert);  // lightmaps[0].primary
                w.u64(kPtrInsert);  // lightmaps[0].secondary
                writeImage(w, gw.lightmapPrimary);
                writeImage(w, gw.lightmapSecondary);
                runtimeArray(w, kAlign4, 8 * 1);  // lightmapPrimaryTextures
                runtimeArray(w, kAlign4, 8 * 1);  // lightmapSecondaryTextures
                // lightmapOverridePrimary/Secondary (+64/+72): null. vertices, layer data, indices.
                if (!gw.vertices.empty()) {
                    w.align(kAlign4);
                    for (const gfx::Vertex& vx : gw.vertices) {
                        iw4::GfxWorldVertex o{};
                        std::memcpy(o.xyz, vx.xyz, sizeof(o.xyz));
                        o.binormalSign = vx.binormalSign;
                        o.color.packed = vx.color;
                        std::memcpy(o.texCoord, vx.texCoord, sizeof(o.texCoord));
                        std::memcpy(o.lmapCoord, vx.lmapCoord, sizeof(o.lmapCoord));
                        o.normal.packed = vx.normal;
                        o.tangent.packed = vx.tangent;
                        w.writeStruct(o);
                    }
                }
                w.align(kAlign1);
                w.zeros(4);
                if (!gw.indices.empty()) {
                    w.align(kAlign2);
                    w.write(gw.indices.data(), gw.indices.size() * 2);
                }
                // lightGrid (0x11aaa0): rowDataStart, rawRowData, entries, colors.
                w.align(kAlign2);
                w.write(lg.rowDataStart.data(), lg.rowDataStart.size() * 2);
                w.align(kAlign1);
                w.write(lg.rawRowData.data(), lg.rawRowData.size());
                w.align(kAlign4);
                if (lg.entries.empty())
                    w.zeros(4ull * lg.entryCount);  // every entry: colors 0, no primary light
                else
                    w.write(lg.entries.data(), 4ull * lg.entries.size());
                w.align(kAlign4);
                if (lg.colors.empty()) {
                    std::vector<uint8_t> color(sizeof(iw4::GfxLightGridColors), lg.gray);
                    for (uint32_t i = 0; i < lg.colorCount; i++) w.write(color.data(), color.size());
                } else {
                    std::vector<uint8_t> color(sizeof(iw4::GfxLightGridColors));
                    for (const auto& rgb : lg.colors) {
                        for (size_t j = 0; j + 2 < color.size(); j += 3) std::memcpy(&color[j], rgb.data(), 3);
                        w.write(color.data(), color.size());
                    }
                }
                // models: the world brush model covers every static surface.
                w.align(kAlign4);
                {
                    iw4::GfxBrushModel m{};
                    m.bounds = g.bounds;
                    m.radius = std::sqrt(g.bounds.halfSize[0] * g.bounds.halfSize[0] +
                                         g.bounds.halfSize[1] * g.bounds.halfSize[1] +
                                         g.bounds.halfSize[2] * g.bounds.halfSize[2]);
                    m.surfaceCount = m.surfaceCountNoDecal = static_cast<unsigned short>(ns);
                    w.writeStruct(m);
                }
                // materialMemory: each material once, as a by-name reference.
                std::vector<uint64_t> matSlot(gw.materials.size());
                if (!gw.materials.empty()) {
                    w.align(kAlign4);
                    for (size_t i = 0; i < gw.materials.size(); i++) {
                        w.u64(kPtrInsert);
                        w.u64(0);  // memory
                    }
                    std::map<std::string, uint64_t> imageSlots;
                    for (size_t i = 0; i < gw.materials.size(); i++) {
                        const std::string full = worldMaterialAssetName(gw.materials[i]);
                        const matdef::Def* def = nullptr;
                        if (materials) {
                            auto it = materials->find(matdef::key(full));
                            if (it != materials->end() && !matdef::techsetFor(it->second).empty()) def = &it->second;
                        }
                        if (hasSky && static_cast<int>(i) == gw.skyMaterial)
                            matSlot[i] = writeSkyMaterial(w, *skySrc, skyImageSlot, gfx::kSkyMaterial);
                        else if (def)
                            matSlot[i] = writeWorldMaterial(w, *def, imageSlots);
                        else
                            matSlot[i] = writeMaterialRef(w, full);
                    }
                }
                // sun (inline, no sprite/flare materials), outdoorImage (null), then RUNTIME bits.
                runtimeArray(w, kAlign4, 4ull * cellCount * ((cellCount + 31) >> 5));  // cellCasterBits
                runtimeArray(w, kAlign4, 4ull * ((cellCount + 31) >> 5));              // cellHasSunLitSurfsBits
                runtimeArray(w, kAlign4, 0);  // sceneDynModel (no dyn ents)
                runtimeArray(w, kAlign4, 0);  // sceneDynBrush
                runtimeArray(w, kAlign4, static_cast<size_t>(primaryLights - 1 - 1) << 15);  // entity shadow vis
                runtimeArray(w, kAlign4, 0);  // primaryLightDynEntShadowVis[0]
                runtimeArray(w, kAlign4, 0);  // primaryLightDynEntShadowVis[1]
                runtimeArray(w, kAlign1, 0);  // nonSunPrimaryLightForModelDynEnt
                // shadowGeom / lightRegion: one empty entry per primary light.
                w.align(kAlign4);
                w.zeros(sizeof(iw4::GfxShadowGeometry) * primaryLights);
                w.align(kAlign4);
                w.zeros(sizeof(iw4::GfxLightRegion) * primaryLights);
                // dpvs (0x11bb30)
                for (int k = 0; k < 3; k++) runtimeArray(w, kAlign1, 0);   // smodelVisData
                for (int k = 0; k < 3; k++) runtimeArray(w, kAlign1, ns);  // surfaceVisData
                if (ns) {
                    w.align(kAlign2);
                    for (int half = 0; half < 2; half++)
                        for (uint32_t i = 0; i < ns; i++) w.write(&i, 2);  // sortedSurfIndex
                    w.align(kAlign4);
                    for (const gfx::Surface& s : gw.surfaces) {
                        iw4::GfxSurface o{};
                        o.tris.vertexLayerData = 0;
                        o.tris.firstVertex = s.firstVertex;
                        o.tris.vertexCount = s.vertexCount;
                        o.tris.triCount = s.triCount;
                        o.tris.baseIndex = s.baseIndex;
                        o.material = ref<iw4::Material>(matSlot[s.material]);
                        o.laf.fields.lightmapIndex = s.sky ? 31 : 0;  // 31 = no lightmap
                        o.laf.fields.reflectionProbeIndex = 0;
                        o.laf.fields.primaryLightIndex = s.primaryLight;
                        o.laf.fields.flags = 1;  // stock world surfaces
                        w.writeStruct(o);
                    }
                    w.align(kAlign4);
                    for (const gfx::Surface& s : gw.surfaces) {
                        iw4::GfxSurfaceBounds b{};
                        for (int k = 0; k < 3; k++) {
                            b.bounds.midPoint[k] = (s.mins[k] + s.maxs[k]) * 0.5f;
                            b.bounds.halfSize[k] = (s.maxs[k] - s.mins[k]) * 0.5f;
                        }
                        w.writeStruct(b);
                    }
                }
                runtimeArray(w, kAlign8, 8ull * ns);           // surfaceMaterials (game-built sort keys)
                runtimeArray(w, 127, 4ull * visCount);         // surfaceCastsSunShadow
                // dpvsDyn (0x11b760): no dyn ents, all sizes 0.
                for (int k = 0; k < 2; k++) runtimeArray(w, kAlign4, 0);
                for (int k = 0; k < 6; k++) runtimeArray(w, kAlign16, 0);
                w.pop();
            }};
}

// Load_GameWorldMp (0x11a1e0): struct 0x10; push VIRTUAL; name; g_glassData (null); pop.
ZoneAsset buildGameWorldMp(const std::string& name) {
    return {IW4_GAMEWORLD_MP, [name](ZoneWriter& w) {
                iw4::GameWorldMp g{};
                g.name = follow<const char>();
                w.writeStruct(g);
                w.push(zone::BLOCK_VIRTUAL);
                w.xstring(name);
                w.pop();
            }};
}

// Load_GameWorldSp (MP 2013 0x11a360, SP 2.0.14 0xddd70, identical): struct 0x70; push VIRTUAL;
// name; PathData inline at +8 (0x11e110: every array pointer checked, nothing read when null);
// vehicleTrack.segments at +0x58 (null); g_glassData at +0x68; pop.
// g_glassData must NOT be null: SP's glass init (iw4sp+0x16ad40) reads ->pieceCount and memsets
// ->glassPieces unconditionally (MP checks for null). So emit an empty one: Load_G_GlassData
// (SP 0xdda40) = struct 0x90, align 4, then glassPieces (+0) and glassNames (+0x18) only if non-null.
ZoneAsset buildGameWorldSp(const std::string& name) {
    return {IW4_GAMEWORLD_SP, [name](ZoneWriter& w) {
                iw4::GameWorldSp g{};
                g.name = follow<const char>();
                g.g_glassData = follow<iw4::G_GlassData>();
                w.writeStruct(g);
                w.push(zone::BLOCK_VIRTUAL);
                w.xstring(name);
                w.align(zone::kAlign4);
                w.writeStruct(iw4::G_GlassData{});  // no pieces, no names
                w.pop();
            }};
}
static_assert(sizeof(iw4::G_GlassData) == 0x90 && offsetof(iw4::G_GlassData, glassNames) == 0x18,
              "G_GlassData layout differs from Load_G_GlassData");
static_assert(sizeof(iw4::GameWorldSp) == 0x70 && offsetof(iw4::GameWorldSp, vehicleTrack) == 0x58 &&
                  offsetof(iw4::GameWorldSp, g_glassData) == 0x68,
              "GameWorldSp layout differs from Load_GameWorldSp");

// Load_FxWorld (0x119ed0): struct 0xb0; push VIRTUAL; name; FxGlassSystem inline (every array
// pointer null = no glass, each one is checked before loading); pop.
ZoneAsset buildFxWorld(const std::string& name) {
    return {IW4_FXWORLD, [name](ZoneWriter& w) {
                iw4::FxWorld f{};
                f.name = follow<const char>();
                w.writeStruct(f);
                w.push(zone::BLOCK_VIRTUAL);
                w.xstring(name);
                w.pop();
            }};
}

// Load_RawFile (0x11e730): struct 0x18; push VIRTUAL; name; buffer (byte-aligned,
// compressedLen bytes, or len+1 when stored uncompressed); pop.
ZoneAsset buildRawFile(const std::string& name, int compressedLen, int len,
                       std::vector<uint8_t> buffer) {
    return {IW4_RAWFILE, [name, compressedLen, len, buffer = std::move(buffer)](ZoneWriter& w) {
                iw4::RawFile r{};
                r.name = follow<const char>();
                r.compressedLen = compressedLen;
                r.len = len;
                r.buffer = buffer.empty() ? nullptr : follow<const char>();
                w.writeStruct(r);
                w.push(zone::BLOCK_VIRTUAL);
                w.xstring(name);
                if (!buffer.empty()) {
                    w.align(zone::kAlign1);
                    w.write(buffer.data(), buffer.size());
                }
                w.pop();
            },
            zone::kPtrFollow};
}

ZoneAsset buildCompressedRawFile(const std::string& name, const std::string& text) {
    // Stock rawfiles with content are zlib streams (compressedLen > 0, data starts 78 da).
    mz_ulong cap = mz_compressBound(static_cast<mz_ulong>(text.size()));
    std::vector<uint8_t> z(cap);
    if (mz_compress2(z.data(), &cap, reinterpret_cast<const unsigned char*>(text.data()),
                     static_cast<mz_ulong>(text.size()), MZ_BEST_COMPRESSION) != MZ_OK)
        cap = 0;
    z.resize(cap);
    const int compressedLen = static_cast<int>(z.size());  // before the move below: argument
    return buildRawFile(name, compressedLen, static_cast<int>(text.size()), std::move(z));  // order is unspecified
}

std::string mapAssetDir(MapTarget target) { return target == MapTarget::SP ? "maps/" : "maps/mp/"; }

std::string mapScriptSource(const std::string& mapName, MapTarget target) {
    // The game runs maps/[mp/]<map>::main() when the map starts; without the script it errors out.
    // SP: just the shared level loader.
    if (target == MapTarget::SP)
        return "// generated by iw4-radiant for " + mapName + "\nmain()\n{\n\tmaps\\_load::main();\n}\n";
    // Minimal MP setup: the shared map loader plus the team roles every stock map sets.
    return "// generated by iw4-radiant for " + mapName +
           "\nmain()\n{\n\tmaps\\mp\\_load::main();\n\n\tgame[\"attackers\"] = \"allies\";\n"
           "\tgame[\"defenders\"] = \"axis\";\n}\n";
}

std::vector<uint8_t> buildZoneImage(const std::vector<ZoneAsset>& assets,
                                    const std::vector<std::string>& scriptStrings) {
    ZoneWriter w;

    // XFile (40 bytes), read directly by DB_LoadXFile; size/blockSize are patched at the end.
    uint8_t xfile[sizeof(iw4::XFile)] = {};
    w.writeUntracked(xfile, sizeof(xfile));

    // XAssetList (32 bytes). Counted in TEMP at the root, per the linker convention.
    const uint32_t stringCount =
        scriptStrings.empty() ? 0 : static_cast<uint32_t>(scriptStrings.size() + 1);
    iw4::XAssetList list{};
    list.stringList.count = static_cast<int>(stringCount);
    list.stringList.strings = stringCount ? follow<const char*>() : nullptr;
    list.assetCount = static_cast<int>(assets.size());
    list.assets = assets.empty() ? nullptr : follow<iw4::XAsset>();
    w.writeStruct(list);

    // Load_ScriptStringList (0x11e930): push VIRTUAL; pointer array (align 4); each -1 string.
    w.push(zone::BLOCK_VIRTUAL);
    if (stringCount) {
        w.align(zone::kAlign4);
        w.u64(0);  // entry 0: the null script string
        for (size_t i = 0; i < scriptStrings.size(); i++) w.u64(zone::kPtrFollow);
        for (const std::string& s : scriptStrings) w.xstring(s);
    }
    w.pop();

    // Asset array (align 4, 16 bytes per XAsset), then each asset via Load_XAsset.
    w.push(zone::BLOCK_VIRTUAL);
    if (!assets.empty()) {
        w.align(zone::kAlign4);
        for (const ZoneAsset& a : assets) {
            w.u32(a.type);
            w.u32(0);
            w.u64(a.header);
        }
        for (const ZoneAsset& a : assets) loadAssetPtr(w, a.header, a.load);
    }
    w.pop();

    std::vector<uint8_t>& z = w.bytes();
    iw4::XFile hdr{};
    hdr.size = static_cast<unsigned int>(z.size() - sizeof(iw4::XFile));
    hdr.externalSize = 0;
    for (int b = 0; b < zone::BLOCK_COUNT; b++) hdr.blockSize[b] = w.blockSize(b);
    std::memcpy(z.data(), &hdr, sizeof(hdr));
    return std::move(z);
}

bool compileMapToFastfile(const Document& doc, const std::string& mapName,
                          std::vector<uint8_t>& fastfileOut, std::string& error, MapTarget target,
                          const CompileOptions& options) {
    if (mapName.empty()) {
        error = "empty map name";
        return false;
    }
    clip::ClipMapData cm;
    if (!clip::buildClipMapData(doc, cm, error)) return false;
    // Lighting: reuse the caller's bake, bake now, or (bakeLighting off) uniform placeholder light.
    light::Bake localBake;
    const light::Bake* bake = options.prebaked;
    if (!bake && options.bakeLighting) {
        if (!light::bake(doc, options.light, localBake, error, options.progress)) {
            error = "lighting: " + error;
            return false;
        }
        bake = &localBake;
    }
    // Sky: copy the stock sky shader set once (cached) if the map has sky faces.
    std::shared_ptr<const sky::Source> skySrc;
    bool wantSky = false;
    for (const Brush& b : doc.ents[0].brushes)
        for (const Face& f : b.faces)
            if (light::isSkyMaterial(f.material)) wantSky = true;
    if (wantSky) {
        auto s = std::make_shared<sky::Source>();
        std::string skyErr;
        if (sky::load(options.gameDir, options.cacheDir, *s, skyErr))
            skySrc = s;
        else if (options.warnings)
            options.warnings->push_back("sky left out: " + skyErr);
    }
    gfx::GfxWorldData gw;
    if (!gfx::buildGfxWorldData(doc, gw, error, bake, skySrc != nullptr)) return false;
    // World materials: embedded in full from the stock definitions; any we don't know stay
    // by-name references (which the game resolves to its default material).
    std::shared_ptr<const matdef::Library> materials = options.materials;
    if (!materials && !options.cacheDir.empty()) {
        auto lib = std::make_shared<matdef::Library>();
        if (matdef::load(options.cacheDir + "/materialdefs.bin", *lib)) materials = lib;
    }
    if (options.warnings) {
        std::string missing;
        int count = 0;
        for (size_t i = 0; i < gw.materials.size(); i++) {
            if (static_cast<int>(i) == gw.skyMaterial) continue;
            const std::string full = worldMaterialAssetName(gw.materials[i]);
            bool known = false;
            if (materials) {
                auto it = materials->find(matdef::key(full));
                known = it != materials->end() && !matdef::techsetFor(it->second).empty();
            }
            if (!known) {
                if (count++ < 8) missing += (missing.empty() ? "" : ", ") + full;
            }
        }
        if (count)
            options.warnings->push_back(std::to_string(count) + " material(s) not embedded (no stock definition" +
                                        std::string(materials ? "" : "; the material library isn't built yet") +
                                        "), they'll show as the default material: " + missing);
    }
    // Every world asset is named after the bsp path the game loads ("maps/mp/<map>.d3dbsp" for
    // MP, "maps/<map>.d3dbsp" for SP).
    const bool sp = target == MapTarget::SP;
    const std::string bsp = mapAssetDir(target) + mapName + ".d3dbsp";
    // Asset order follows stock map zones: ComWorld, FxWorld, GfxWorld, GameWorld, then clipMap
    // last with the entity lump (MapEnts) inline through clipMap_t.mapEnts.
    auto shared = std::make_shared<SharedWorld>();
    // SP and MP clipMaps are the same struct and loader; only the asset type differs.
    ZoneAsset clipMap = buildClipMap(bsp, cm, documentToEntityString(doc), shared, sp);
    if (sp) clipMap.type = IW4_CLIPMAP_SP;
    std::vector<ZoneAsset> assets = {
        buildComWorld(bsp, true, bake ? &bake->env : nullptr),
        buildFxWorld(bsp),
        buildGfxWorld(bsp, mapName, gw, cm, shared, skySrc, materials),
        sp ? buildGameWorldSp(bsp) : buildGameWorldMp(bsp),
        std::move(clipMap),
        buildCompressedRawFile(mapAssetDir(target) + mapName + ".gsc", mapScriptSource(mapName, target)),
    };
    std::vector<uint8_t> image = buildZoneImage(assets);

    Fastfile ff;
    defaultPrefix(ff.prefix);
    ff.zone = std::move(image);
    return writeFastfile(ff, fastfileOut, error);
}
