#include "zonesim.h"
#include <cstring>
#include <deque>
#include <malloc.h>
#include <stdexcept>
#include "iw4.h"
#include "zonewriter.h"

namespace zone {

struct SimulatedZone::Owned {
    struct Aligned {
        void* p = nullptr;
        explicit Aligned(size_t n) : p(_aligned_malloc(n ? n : 1, 4096)) { std::memset(p, 0, n ? n : 1); }
        ~Aligned() { _aligned_free(p); }
        Aligned(const Aligned&) = delete;
        Aligned& operator=(const Aligned&) = delete;
    };
    std::deque<Aligned> chunks;  // blocks, then pooled asset structs
};

const void* SimulatedZone::find(uint32_t type) const {
    for (const LoadedAsset& a : assets)
        if (a.type == type && !a.reference) return a.header;
    return nullptr;
}

size_t SimulatedZone::count(uint32_t type, bool references) const {
    size_t n = 0;
    for (const LoadedAsset& a : assets) n += a.type == type && a.reference == references;
    return n;
}

namespace {

constexpr uint64_t kFollow = ~0ull, kInsert = ~1ull;

struct Fail : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Loader {
public:
    Loader(const std::vector<uint8_t>& img, SimulatedZone& z) : img_(img), z_(z) {}

    void run() {
        if (img_.size() < sizeof(iw4::XFile) + sizeof(iw4::XAssetList)) throw Fail("zone image too small");
        iw4::XFile xf;
        std::memcpy(&xf, img_.data(), sizeof(xf));
        if (xf.size != img_.size() - sizeof(xf)) throw Fail("XFile.size != image size - 40");
        rpos_ = sizeof(xf);
        z_.memory = std::make_shared<SimulatedZone::Owned>();
        for (int b = 0; b < BLOCK_COUNT; b++) {
            z_.blockSize[b] = xf.blockSize[b];
            z_.memory->chunks.emplace_back(xf.blockSize[b]);
            base_[b] = static_cast<uint8_t*>(z_.memory->chunks.back().p);
        }

        // DB_LoadXFile (0x117d40): the XAssetList is read directly, not through a block.
        iw4::XAssetList list;
        readFile(&list, sizeof(list));

        push(BLOCK_VIRTUAL);  // Load_ScriptStringList (0x11e930)
        if (list.stringList.strings) {
            auto* arr = static_cast<uint64_t*>(stream(alloc(kAlign4), 8ull * list.stringList.count));
            for (int i = 0; i < list.stringList.count; i++) {
                if (arr[i] == kFollow) {
                    arr[i] = reinterpret_cast<uint64_t>(xstringData());
                    z_.scriptStrings.push_back(reinterpret_cast<const char*>(arr[i]));
                } else if (arr[i]) {
                    arr[i] = reinterpret_cast<uint64_t>(offsetToPointer(arr[i]));
                    z_.scriptStrings.push_back(reinterpret_cast<const char*>(arr[i]));
                } else {
                    z_.scriptStrings.emplace_back();
                }
            }
        }
        pop();

        push(BLOCK_VIRTUAL);
        if (list.assets) {
            auto* arr = static_cast<iw4::XAsset*>(stream(alloc(kAlign4), 16ull * list.assetCount));
            for (int i = 0; i < list.assetCount; i++) loadXAsset(arr[i]);
        }
        pop();
        if (!stack_.empty()) throw Fail("stream stack not empty at end");
        if (rpos_ != img_.size())
            throw Fail("unconsumed file bytes: read " + std::to_string(rpos_) + " of " + std::to_string(img_.size()));
        for (int b = 0; b < BLOCK_COUNT; b++) z_.blockUsed[b] = high_[b];
    }

private:
    // ---- stream primitives (DB_PushStreamPos / Pop / AllocStreamPos / Load_Stream) ----
    void push(int b) {
        stack_.push_back({cur_, 0});
        cur_ = b;
        stack_.back().pos = pos_[cur_];
    }
    void pop() {
        if (stack_.empty()) throw Fail("stream stack underflow");
        auto s = stack_.back();
        stack_.pop_back();
        if (cur_ == BLOCK_TEMP) pos_[BLOCK_TEMP] = s.pos;
        cur_ = s.block;
    }
    uint8_t* alloc(uint32_t mask) {
        pos_[cur_] = (pos_[cur_] + mask) & ~mask;
        return base_[cur_] + pos_[cur_];
    }
    void advance(size_t n) {
        if (pos_[cur_] + n > z_.blockSize[cur_])
            throw Fail("block " + std::to_string(cur_) + " overrun: need " + std::to_string(pos_[cur_] + n) +
                       ", blockSize " + std::to_string(z_.blockSize[cur_]));
        pos_[cur_] += static_cast<uint32_t>(n);
        if (pos_[cur_] > high_[cur_]) high_[cur_] = pos_[cur_];
    }
    void readFile(void* dst, size_t n) {
        if (rpos_ + n > img_.size()) throw Fail("read past end of zone image");
        std::memcpy(dst, img_.data() + rpos_, n);
        rpos_ += n;
    }
    // Load_Stream(true, p, n): p must be the current position.
    void* stream(uint8_t* p, size_t n) {
        if (p != base_[cur_] + pos_[cur_]) throw Fail("Load_Stream not at the stream position");
        size_t at = pos_[cur_];
        advance(n);
        if (cur_ == BLOCK_RUNTIME)
            std::memset(base_[cur_] + at, 0, n);
        else
            readFile(base_[cur_] + at, n);
        return p;
    }
    uint8_t* insertPointer() {  // DB_InsertPointer (0x132080)
        int prev = cur_;
        cur_ = BLOCK_VIRTUAL;
        uint8_t* slot = alloc(kAlign8);
        advance(8);
        cur_ = prev;
        return slot;
    }
    void* offsetToPointer(uint64_t p) {  // DB_ConvertOffsetToPointer (0x132270)
        uint64_t v = p - 1;
        uint64_t block = v >> 28, off = v & 0x0FFFFFFF;
        if (block >= BLOCK_COUNT || off >= z_.blockSize[block])
            throw Fail("back-reference out of range: block " + std::to_string(block) + " offset " + std::to_string(off));
        // Partial loads: earlier assets weren't loaded, so their memory is zero (null asset
        // pointers through insert slots, empty strings) rather than an error.
        if (off >= high_[block] && !lenient_) throw Fail("back-reference to data not loaded yet");
        return base_[block] + off;
    }
    // Partial loads: force the VIRTUAL position where a known array is about to be allocated.
    void applyAnchor() {
        if (!anchorPending_) return;
        anchorPending_ = false;
        pos_[BLOCK_VIRTUAL] = anchorValue_;
        if (anchorValue_ > high_[BLOCK_VIRTUAL]) high_[BLOCK_VIRTUAL] = anchorValue_;
    }

public:
    void runPartial(const std::vector<PartialAsset>& assets, std::vector<PartialResult>& results) {
        iw4::XFile xf;
        if (img_.size() < sizeof(xf)) throw Fail("zone image too small");
        std::memcpy(&xf, img_.data(), sizeof(xf));
        z_.memory = std::make_shared<SimulatedZone::Owned>();
        for (int b = 0; b < BLOCK_COUNT; b++) {
            z_.blockSize[b] = xf.blockSize[b];
            z_.memory->chunks.emplace_back(xf.blockSize[b]);
            base_[b] = static_cast<uint8_t*>(z_.memory->chunks.back().p);
        }
        lenient_ = true;
        for (const PartialAsset& a : assets) {
            rpos_ = a.fileOffset;
            anchorPending_ = a.hasAnchor;
            anchorValue_ = a.anchor;
            stopped_.clear();
            rawRefs_.clear();
            mapEntsAt_ = 0;
            iw4::XAsset x{};
            x.type = static_cast<iw4::XAssetType>(a.type);
            x.header.data = reinterpret_cast<void*>(static_cast<uintptr_t>(kFollow));
            push(BLOCK_VIRTUAL);
            loadXAsset(x);
            pop();
            PartialResult r;
            r.endOffset = rpos_;
            r.stopped = stopped_;
            r.rawRefs = rawRefs_;
            r.mapEntsOffset = mapEntsAt_;
            results.push_back(std::move(r));
        }
        for (int b = 0; b < BLOCK_COUNT; b++) z_.blockUsed[b] = high_[b];
    }

private:
    // Load_XString (0x1322f0 / 0x132380): byte-aligned, NUL-terminated.
    const char* xstringData() {
        uint8_t* p = alloc(kAlign1);
        size_t n = 0;
        do {
            stream(p + n, 1);
        } while (p[n++] != 0);
        return reinterpret_cast<const char*>(p);
    }
    template <class T>
    void xstring(T*& field) {
        uint64_t raw = reinterpret_cast<uint64_t>(field);
        if (!raw) return;
        if (raw == kFollow)
            field = reinterpret_cast<T*>(const_cast<char*>(xstringData()));
        else
            field = reinterpret_cast<T*>(offsetToPointer(raw));
    }
    // An array pointer the loader loads whenever it is non-null.
    template <class T>
    T* array(T*& field, size_t count, uint32_t mask) {
        if (!field) return nullptr;
        field = static_cast<T*>(stream(alloc(mask), sizeof(T) * count));
        return field;
    }
    // A pointer that is -1 (load `count` elements inline) or else a back-reference.
    template <class T>
    T* inlineOrRef(T*& field, size_t count, uint32_t mask) {
        uint64_t raw = reinterpret_cast<uint64_t>(field);
        if (!raw) return nullptr;
        if (raw == kFollow)
            field = static_cast<T*>(stream(alloc(mask), sizeof(T) * count));
        else
            field = static_cast<T*>(offsetToPointer(raw));
        return field;
    }
    void requireNull(const void* p, const char* what) {
        if (p) throw Fail(std::string("zonesim: unsupported non-null ") + what);
    }

    // ---- asset pointers ----
    // Load_<Asset>Ptr: push TEMP; -1/-2 load inline (+insert slot for -2) and DB_Add; else the
    // value is a back-reference to an insert slot holding the asset pointer.
    template <class T, class Body>
    void assetPtr(T*& slot, uint32_t type, Body body) {
        push(BLOCK_TEMP);
        uint64_t raw = reinterpret_cast<uint64_t>(slot);
        if (raw) {
            if (raw == kFollow || raw == kInsert) {
                uint8_t* at = alloc(kAlign4);
                uint8_t* ins = raw == kInsert ? insertPointer() : nullptr;
                T* s = static_cast<T*>(stream(at, sizeof(T)));
                body(*s);
                z_.memory->chunks.emplace_back(sizeof(T));  // DB_Add*: copy out of TEMP
                T* added = static_cast<T*>(z_.memory->chunks.back().p);
                std::memcpy(added, s, sizeof(T));
                // Every asset struct starts with (or, for GfxImage, holds) its name; a leading ','
                // makes DB_AddXAsset (0x12e700) look the asset up instead of adding it.
                const char* nm = assetName(type, added);
                z_.assets.push_back({type, added, nm && nm[0] == ','});
                slot = added;
                if (ins) std::memcpy(ins, &added, 8);
            } else {
                slot = *static_cast<T**>(offsetToPointer(raw));
            }
        }
        pop();
    }

    void loadXAsset(iw4::XAsset& a) {  // Load_XAsset (0x122df0) -> switch at 0x122e30
        switch (a.type) {
            case 16: assetPtr(a.header.comWorld, a.type, [&](iw4::ComWorld& c) { comWorld(c); }); break;
            case 17: assetPtr(a.header.gameWorldSp, a.type, [&](iw4::GameWorldSp& g) { gameWorldSp(g); }); break;
            case 18: assetPtr(a.header.gameWorldMp, a.type, [&](iw4::GameWorldMp& g) { gameWorldMp(g); }); break;
            case 19: assetPtr(a.header.mapEnts, a.type, [&](iw4::MapEnts& m) { mapEnts(m); }); break;
            case 20: assetPtr(a.header.fxWorld, a.type, [&](iw4::FxWorld& f) { fxWorld(f); }); break;
            case 21: assetPtr(a.header.gfxWorld, a.type, [&](iw4::GfxWorld& g) { gfxWorld(g); }); break;
            case 14:
            case 15: assetPtr(a.header.clipMap, a.type, [&](iw4::clipMap_t& c) { clipMap(c); }); break;
            case 36: assetPtr(a.header.rawfile, a.type, [&](iw4::RawFile& r) { rawFile(r); }); break;
            default: throw Fail("zonesim: unsupported asset type " + std::to_string(a.type));
        }
    }

    static const char* assetName(uint32_t type, const void* header) {
        if (type == 10) return static_cast<const iw4::GfxImage*>(header)->name;  // name at +0x20
        const char* n;
        std::memcpy(&n, header, sizeof(n));  // every other asset type: name pointer at +0
        return n;
    }

    void comWorld(iw4::ComWorld& c) {  // 0x1188a0
        push(BLOCK_VIRTUAL);
        xstring(c.name);
        if (auto* l = array(c.primaryLights, c.primaryLightCount, kAlign4))
            for (unsigned i = 0; i < c.primaryLightCount; i++) xstring(l[i].defName);
        pop();
    }

    // Load_GfxImagePtr (0x11a860) / Load_GfxImage (0x11a7b0) / Load_GfxImageLoadDefPtr (0x11ad80).
    void imagePtr(iw4::GfxImage*& slot) {
        assetPtr(slot, 10, [&](iw4::GfxImage& im) {
            push(BLOCK_VIRTUAL);
            xstring(im.name);
            push(BLOCK_TEMP);
            uint64_t raw = reinterpret_cast<uint64_t>(im.texture.loadDef);
            if (raw == kFollow || raw == kInsert) {
                uint8_t* at = alloc(kAlign4);
                uint8_t* ins = raw == kInsert ? insertPointer() : nullptr;
                stream(at, 16);
                uint32_t size;
                std::memcpy(&size, at + 12, 4);
                stream(at + 16, size);
                // Image_LoadFromData (0x37b00) uploads the pixels; keep a copy for inspection.
                z_.memory->chunks.emplace_back(16 + size);
                std::memcpy(z_.memory->chunks.back().p, at, 16 + size);
                im.texture.loadDef = static_cast<iw4::GfxImageLoadDef*>(z_.memory->chunks.back().p);
                if (ins) std::memcpy(ins, &im.texture.loadDef, 8);
            } else if (raw) {
                im.texture.loadDef = *static_cast<iw4::GfxImageLoadDef**>(offsetToPointer(raw));
            }
            pop();
            pop();
        });
    }
    // Load_MaterialHandle (0x11d120) / Load_Material (0x11cf00). Only by-name stubs are supported:
    // techset, texture/constant/stateBits tables must be null.
    void materialPtr(iw4::Material*& slot) {
        assetPtr(slot, 5, [&](iw4::Material& m) {
            uint8_t* s = reinterpret_cast<uint8_t*>(&m);
            push(BLOCK_VIRTUAL);
            xstring(m.info.name);  // Load_MaterialInfo (0x11d2e0)
            techsetPtr(s + 88);    // Load_MaterialTechniqueSetPtr (0x11d8b0)
            // textureTable: 16-byte MaterialTextureDef; semantic (+7) 11 = water_t at +8, else image.
            if (uint8_t* tt = rawInlineOrRef(s + 96, 16ull * s[80], kAlign4))
                for (int k = 0; k < s[80]; k++) {
                    uint8_t* e = tt + 16 * k;
                    if (e[7] == 11) {
                        if (uint8_t* w = rawInlineOrRef(e + 8, 0x60, kAlign4)) {  // Load_water_t (0x126980)
                            int32_t wm, wn;
                            std::memcpy(&wm, w + 32, 4);
                            std::memcpy(&wn, w + 36, 4);
                            for (int a = 0; a < 3; a++) rawArray(w + 8 + 8 * a, 4ull * static_cast<uint32_t>(wm) * static_cast<uint32_t>(wn), kAlign4);
                            imagePtr(*reinterpret_cast<iw4::GfxImage**>(w + 88));
                        }
                    } else {
                        imagePtr(*reinterpret_cast<iw4::GfxImage**>(e + 8));
                    }
                }
            rawInlineOrRef(s + 104, 32ull * s[81], kAlign16);  // constantTable
            rawInlineOrRef(s + 112, 8ull * s[82], kAlign4);    // stateBitsTable
            pop();
        });
    }

    // ---- raw-offset helpers for structs whose header layout we don't rely on ----
    static uint64_t rd64(const uint8_t* p) {
        uint64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }
    static void wr64(uint8_t* p, const void* v) { std::memcpy(p, &v, 8); }
    // -1: load `bytes` inline (returns the data, freshly loaded); other non-zero: back-reference
    // (field resolved, returns nullptr so callers don't walk shared data twice); 0: nullptr.
    uint8_t* rawInlineOrRef(uint8_t* field, size_t bytes, uint32_t mask) {
        uint64_t raw = rd64(field);
        if (!raw) return nullptr;
        if (raw == kFollow) {
            uint8_t* p = static_cast<uint8_t*>(stream(alloc(mask), bytes));
            wr64(field, p);
            return p;
        }
        wr64(field, offsetToPointer(raw));
        return nullptr;
    }
    // Load_<X>Ptr by raw offset (same protocol as assetPtr).
    template <class Body>
    void rawAssetPtr(uint8_t* field, uint32_t type, size_t structSize, Body body) {
        push(BLOCK_TEMP);
        uint64_t raw = rd64(field);
        if (raw) {
            if (raw == kFollow || raw == kInsert) {
                uint8_t* at = alloc(kAlign4);
                uint8_t* ins = raw == kInsert ? insertPointer() : nullptr;
                uint8_t* s = static_cast<uint8_t*>(stream(at, structSize));
                body(s);
                z_.memory->chunks.emplace_back(structSize);
                uint8_t* added = static_cast<uint8_t*>(z_.memory->chunks.back().p);
                std::memcpy(added, s, structSize);
                const char* nm = reinterpret_cast<const char*>(rd64(added));
                z_.assets.push_back({type, added, nm && nm[0] == ','});
                wr64(field, added);
                if (ins) wr64(ins, added);
            } else {
                wr64(field, reinterpret_cast<void*>(rd64(static_cast<uint8_t*>(offsetToPointer(raw)))));
            }
        }
        pop();
    }
    void rawXString(uint8_t* field) {
        const char* p = reinterpret_cast<const char*>(rd64(field));
        xstring(p);
        wr64(field, p);
    }
    // Vertex/pixel shader (0x11daf0 / 0x11d4c0): struct 0x20; name; program at +16, its size in
    // dwords (u16) at +24.
    void shaderPtr(uint8_t* field, uint32_t type) {
        rawAssetPtr(field, type, 0x20, [&](uint8_t* s) {
            push(BLOCK_VIRTUAL);
            rawXString(s);
            uint16_t dwords;
            std::memcpy(&dwords, s + 24, 2);
            rawArray(s + 16, 4ull * dwords, kAlign4);
            pop();
        });
    }
    // Technique set (0x11d790): struct 0x198; name; 48 technique pointers at +24. A technique
    // (0x11d6b0) is a 16-byte header + passCount (u16 @+10) 40-byte passes, then its name.
    void techsetPtr(uint8_t* field) {
        rawAssetPtr(field, 9, 0x198, [&](uint8_t* ts) {
            push(BLOCK_VIRTUAL);
            rawXString(ts);
            for (int t = 0; t < 48; t++) {
                uint8_t* f = ts + 24 + 8 * t;
                uint64_t raw = rd64(f);
                if (!raw) continue;
                if (raw != kFollow) {
                    wr64(f, offsetToPointer(raw));
                    continue;
                }
                uint8_t* tech = alloc(kAlign4);
                stream(tech, 16);
                uint16_t passes;
                std::memcpy(&passes, tech + 10, 2);
                stream(tech + 16, 40ull * passes);
                wr64(f, tech);
                for (uint16_t k = 0; k < passes; k++) {  // Load_MaterialPass (0x11d360)
                    uint8_t* pass = tech + 16 + 40 * k;
                    rawAssetPtr(pass + 0, 8, 0xB0, [&](uint8_t* vd) {  // vertex decl (0x11d970)
                        push(BLOCK_VIRTUAL);
                        rawXString(vd);
                        pop();
                    });
                    shaderPtr(pass + 8, 7);
                    shaderPtr(pass + 16, 6);
                    size_t argc = static_cast<size_t>(pass[24]) + pass[25] + pass[26];
                    rawArray(pass + 32, 16 * argc, kAlign4);  // any non-null value loads inline
                    if (uint8_t* args = reinterpret_cast<uint8_t*>(rd64(pass + 32)))
                        for (size_t a = 0; a < argc; a++) {
                            uint16_t type;
                            std::memcpy(&type, args + 16 * a, 2);
                            if (type == 1 || type == 7) rawInlineOrRef(args + 16 * a + 8, 16, kAlign4);  // literal
                        }
                }
                rawXString(tech);
            }
            pop();
        });
    }
    // A pointer field addressed by raw offset: loaded whenever non-null.
    void rawArray(uint8_t* field, size_t bytes, uint32_t mask) {
        uint64_t raw;
        std::memcpy(&raw, field, 8);
        if (!raw) return;
        void* p = stream(alloc(mask), bytes);
        std::memcpy(field, &p, 8);
    }
    // RUNTIME arrays inside a push(RUNTIME)/pop.
    template <class T>
    void runtime(T*& field, size_t bytes, uint32_t mask) {
        push(BLOCK_RUNTIME);
        if (field) field = static_cast<T*>(stream(alloc(mask), bytes));
        pop();
    }

    void gfxWorld(iw4::GfxWorld& g) {  // Load_GfxWorld (0x11ae60), in its exact order
        push(BLOCK_VIRTUAL);
        xstring(g.name);
        xstring(g.baseName);
        if (auto* s = array(g.skies, g.skyCount, kAlign4))
            for (int i = 0; i < g.skyCount; i++) {
                array(s[i].skyStartSurfs, s[i].skySurfCount, kAlign4);
                imagePtr(s[i].skyImage);
            }
        // dpvsPlanes (0x11ba20)
        if (reinterpret_cast<uint64_t>(g.dpvsPlanes.planes) == kFollow) applyAnchor();
        inlineOrRef(g.dpvsPlanes.planes, g.planeCount, kAlign4);
        array(g.dpvsPlanes.nodes, g.nodeCount, kAlign2);
        runtime(g.dpvsPlanes.sceneEntCellBits, static_cast<size_t>(g.dpvsPlanes.cellCount) << 11, kAlign4);
        const int cells = g.dpvsPlanes.cellCount;
        array(g.aabbTreeCounts, cells, kAlign4);
        if (auto* ct = array(g.aabbTrees, cells, 127))  // 0x11a690 per cell
            for (int c = 0; c < cells; c++)
                if (auto* t = array(ct[c].aabbTree, g.aabbTreeCounts[c].aabbTreeCount, kAlign4))
                    for (int k = 0; k < g.aabbTreeCounts[c].aabbTreeCount; k++)
                        inlineOrRef(t[k].smodelIndexes, t[k].smodelIndexCount, kAlign2);
        if (auto* cl = array(g.cells, cells, kAlign4))  // 0x11a550
            for (int c = 0; c < cells; c++) {
                if (auto* p = reinterpret_cast<uint8_t*>(array(cl[c].portals, cl[c].portalCount, kAlign4)))
                    for (int k = 0; k < cl[c].portalCount; k++)  // 80-byte GfxPortal: verts @+40, count u8 @+50
                        rawArray(p + 80 * k + 40, 12ull * p[80 * k + 50], kAlign4);
                array(cl[c].reflectionProbes, static_cast<unsigned char>(cl[c].reflectionProbeCount), kAlign1);
            }
        // draw (0x11bfd0)
        iw4::GfxWorldDraw& d = g.draw;
        if (auto* rp = array(d.reflectionProbes, d.reflectionProbeCount, kAlign4))
            for (unsigned i = 0; i < d.reflectionProbeCount; i++) imagePtr(rp[i]);
        array(d.reflectionProbeOrigins, d.reflectionProbeCount, kAlign4);
        runtime(d.reflectionProbeTextures, 8ull * d.reflectionProbeCount, kAlign4);
        if (auto* lm = array(d.lightmaps, d.lightmapCount, kAlign4))
            for (int i = 0; i < d.lightmapCount; i++) {
                imagePtr(lm[i].primary);
                imagePtr(lm[i].secondary);
            }
        runtime(d.lightmapPrimaryTextures, 8ull * d.lightmapCount, kAlign4);
        runtime(d.lightmapSecondaryTextures, 8ull * d.lightmapCount, kAlign4);
        imagePtr(d.lightmapOverridePrimary);
        imagePtr(d.lightmapOverrideSecondary);
        array(d.vd.vertices, d.vertexCount, kAlign4);  // 0x11c3d0 (+ D3D buffer from it)
        if (d.vld.data)                                // 0x11c470
            d.vld.data = static_cast<char*>(stream(alloc(kAlign1), d.vertexLayerDataSize));
        array(d.indices, d.indexCount, kAlign2);
        // lightGrid (0x11aaa0)
        iw4::GfxLightGrid& lg = g.lightGrid;
        if (lg.rowDataStart) {
            size_t n = static_cast<size_t>(lg.maxs[lg.rowAxis] - lg.mins[lg.rowAxis]) + 1;
            lg.rowDataStart = static_cast<unsigned short*>(stream(alloc(kAlign2), 2 * n));
        }
        if (lg.rawRowData) lg.rawRowData = static_cast<char*>(stream(alloc(kAlign1), lg.rawRowDataSize));
        array(lg.entries, lg.entryCount, kAlign4);
        array(lg.colors, lg.colorCount, kAlign4);
        array(g.models, g.modelCount, kAlign4);
        if (auto* mm = array(g.materialMemory, g.materialMemoryCount, kAlign4))
            for (int i = 0; i < g.materialMemoryCount; i++) materialPtr(mm[i].material);
        materialPtr(g.sun.spriteMaterial);
        materialPtr(g.sun.flareMaterial);
        imagePtr(g.outdoorImage);
        const unsigned cells32 = (static_cast<unsigned>(cells) + 31) >> 5;
        const unsigned nonSun = g.primaryLightCount - g.lastSunPrimaryLightIndex - 1;
        runtime(g.cellCasterBits, 4ull * cells * cells32, kAlign4);
        runtime(g.cellHasSunLitSurfsBits, 4ull * cells32, kAlign4);
        runtime(g.sceneDynModel, 6ull * g.dpvsDyn.dynEntClientCount[0], kAlign4);
        runtime(g.sceneDynBrush, 4ull * g.dpvsDyn.dynEntClientCount[1], kAlign4);
        runtime(g.primaryLightEntityShadowVis, static_cast<size_t>(nonSun) << 15, kAlign4);
        runtime(g.primaryLightDynEntShadowVis[0], 4ull * g.dpvsDyn.dynEntClientCount[0] * nonSun, kAlign4);
        runtime(g.primaryLightDynEntShadowVis[1], 4ull * g.dpvsDyn.dynEntClientCount[1] * nonSun, kAlign4);
        runtime(g.nonSunPrimaryLightForModelDynEnt, g.dpvsDyn.dynEntClientCount[0], kAlign1);
        if (auto* sg = array(g.shadowGeom, g.primaryLightCount, kAlign4))  // 0x11acd0
            for (unsigned i = 0; i < g.primaryLightCount; i++) {
                array(sg[i].sortedSurfIndex, sg[i].surfaceCount, kAlign2);
                array(sg[i].smodelIndex, sg[i].smodelCount, kAlign2);
            }
        if (auto* lr = array(g.lightRegion, g.primaryLightCount, kAlign4))  // 0x11abe0
            for (unsigned i = 0; i < g.primaryLightCount; i++)
                if (auto* hl = reinterpret_cast<uint8_t*>(array(lr[i].hulls, lr[i].hullCount, kAlign4)))
                    for (unsigned k = 0; k < lr[i].hullCount; k++) {  // 88-byte hull: axis @+80, count @+72
                        uint32_t n;
                        std::memcpy(&n, hl + 88 * k + 72, 4);
                        rawArray(hl + 88 * k + 80, 20ull * n, kAlign4);
                    }
        // dpvs (0x11bb30)
        iw4::GfxWorldDpvsStatic& v = g.dpvs;
        for (int k = 0; k < 3; k++) runtime(v.smodelVisData[k], v.smodelCount, kAlign1);
        for (int k = 0; k < 3; k++) runtime(v.surfaceVisData[k], v.staticSurfaceCount, kAlign1);
        array(v.sortedSurfIndex, static_cast<size_t>(v.staticSurfaceCount) + v.staticSurfaceCountNoDecal, kAlign2);
        array(v.smodelInsts, v.smodelCount, kAlign4);
        if (auto* s = array(v.surfaces, g.surfaceCount, kAlign4))
            for (unsigned i = 0; i < g.surfaceCount; i++) materialPtr(s[i].material);
        array(v.surfacesBounds, g.surfaceCount, kAlign4);
        if (v.smodelDrawInsts && lenient_) {
            // Static model draw instances carry inline XModels, which this port doesn't load.
            stopped_ = "GfxWorld.dpvs.smodelDrawInsts (inline XModels)";
            pop();
            return;
        }
        requireNull(v.smodelDrawInsts, "dpvs.smodelDrawInsts");
        runtime(v.surfaceMaterials, 8ull * g.surfaceCount, kAlign8);
        runtime(v.surfaceCastsSunShadow, 4ull * v.surfaceVisDataCount, 127);
        // dpvsDyn (0x11b760)
        iw4::GfxWorldDpvsDynamic& dy = g.dpvsDyn;
        runtime(dy.dynEntCellBits[0], 4ull * dy.dynEntClientWordCount[0] * cells, kAlign4);
        runtime(dy.dynEntCellBits[1], 4ull * dy.dynEntClientWordCount[1] * cells, kAlign4);
        for (int j = 0; j < 3; j++) {
            runtime(dy.dynEntVisData[0][j], 32ull * dy.dynEntClientWordCount[0], kAlign16);
            runtime(dy.dynEntVisData[1][j], 32ull * dy.dynEntClientWordCount[1], kAlign16);
        }
        array(g.heroOnlyLights, g.heroOnlyLightCount, kAlign4);
        pop();
    }
    void gameWorldMp(iw4::GameWorldMp& g) {  // 0x11a1e0
        push(BLOCK_VIRTUAL);
        xstring(g.name);
        requireNull(g.g_glassData, "GameWorldMp.g_glassData");
        pop();
    }
    void gameWorldSp(iw4::GameWorldSp& g) {  // 0x11a360 (SP 0xddd70)
        push(BLOCK_VIRTUAL);
        xstring(g.name);
        requireNull(g.path.nodes, "GameWorldSp.path.nodes");
        requireNull(g.path.basenodes, "GameWorldSp.path.basenodes");
        requireNull(g.path.chainNodeForNode, "GameWorldSp.path.chainNodeForNode");
        requireNull(g.path.nodeForChainNode, "GameWorldSp.path.nodeForChainNode");
        requireNull(g.path.pathVis, "GameWorldSp.path.pathVis");
        requireNull(g.path.nodeTree, "GameWorldSp.path.nodeTree");
        requireNull(g.vehicleTrack.segments, "GameWorldSp.vehicleTrack.segments");
        if (auto* gd = array(g.g_glassData, 1, kAlign4)) {  // Load_G_GlassData (SP 0xdda40)
            requireNull(gd->glassPieces, "GameWorldSp.g_glassData.glassPieces");
            requireNull(gd->glassNames, "GameWorldSp.g_glassData.glassNames");
        }
        pop();
    }
    void fxWorld(iw4::FxWorld& f) {  // 0x119ed0 + Load_FxGlassSystem 0x119740
        push(BLOCK_VIRTUAL);
        xstring(f.name);
        const uint8_t* gs = reinterpret_cast<const uint8_t*>(&f.glassSys);
        for (size_t off = 48; off <= 144; off += 8) {
            uint64_t p;
            std::memcpy(&p, gs + off, 8);
            if (p) throw Fail("zonesim: unsupported FxWorld glass data");
        }
        pop();
    }
    void mapEnts(iw4::MapEnts& m) {  // 0x11cb80
        push(BLOCK_VIRTUAL);
        xstring(m.name);
        if (m.entityString)
            m.entityString = static_cast<char*>(stream(alloc(kAlign1), static_cast<size_t>(m.numEntityChars)));
        // Load_MapTriggers (0x11ce10): models 8, hulls 32, slabs 20 bytes each; then stages (24
        // bytes, name at +0), stageCount u8.
        uint8_t* t = reinterpret_cast<uint8_t*>(&m.trigger);
        uint32_t models, hulls, slabs;
        std::memcpy(&models, t + 0, 4);
        std::memcpy(&hulls, t + 16, 4);
        std::memcpy(&slabs, t + 32, 4);
        rawArray(t + 8, 8ull * models, kAlign4);
        rawArray(t + 24, 32ull * hulls, kAlign4);
        rawArray(t + 40, 20ull * slabs, kAlign4);
        uint8_t* me = reinterpret_cast<uint8_t*>(&m);
        const unsigned stages = me[0x50];  // stageCount (u8) at +0x50, stages pointer at +0x48
        rawArray(me + 0x48, 24ull * stages, kAlign4);
        if (uint8_t* st = reinterpret_cast<uint8_t*>(rd64(me + 0x48)))
            for (unsigned i = 0; i < stages; i++) rawXString(st + 24 * i);
        pop();
    }
    void rawFile(iw4::RawFile& r) {  // 0x11e730
        push(BLOCK_VIRTUAL);
        xstring(r.name);
        if (r.buffer) {
            size_t n = r.compressedLen ? static_cast<size_t>(r.compressedLen) : static_cast<size_t>(r.len) + 1;
            r.buffer = static_cast<const char*>(stream(alloc(kAlign1), n));
        }
        pop();
    }
    void plane1(iw4::cplane_s*& p) { inlineOrRef(p, 1, kAlign4); }

    void clipMap(iw4::clipMap_t& c) {  // Load_clipMap_t (0x1242b0), in its exact order
        push(BLOCK_VIRTUAL);
        xstring(c.name);
        inlineOrRef(c.planes, c.planeCount, kAlign4);
        // Static models: 80 bytes, XModel pointer at +0 (Load_XModelPtr 0x123700). Only
        // back-references (models loaded earlier in the zone) are supported.
        if (auto* sm = reinterpret_cast<uint8_t*>(array(c.staticModelList, c.numStaticModels, kAlign4)))
            for (unsigned i = 0; i < c.numStaticModels; i++)
                rawAssetPtr(sm + 80 * i, 4, 0, [&](uint8_t*) { throw Fail("zonesim: unsupported inline XModel"); });
        if (auto* m = array(c.materials, c.numMaterials, kAlign4))
            for (unsigned i = 0; i < c.numMaterials; i++) xstring(m[i].name);
        if (c.brushsides) applyAnchor();
        if (auto* s = array(c.brushsides, c.numBrushSides, kAlign4))  // 0x124140
            for (unsigned i = 0; i < c.numBrushSides; i++) plane1(s[i].plane);
        array(c.brushEdges, c.numBrushEdges, kAlign1);
        if (auto* n = array(c.nodes, c.numNodes, kAlign4))
            for (unsigned i = 0; i < c.numNodes; i++) plane1(n[i].plane);
        array(c.leafs, c.numLeafs, kAlign4);
        array(c.leafbrushes, c.numLeafBrushes, kAlign2);
        if (auto* n = array(c.leafbrushNodes, c.leafbrushNodesCount, kAlign4))
            for (unsigned i = 0; i < c.leafbrushNodesCount; i++)
                if (n[i].leafBrushCount > 0)
                    inlineOrRef(n[i].data.leaf.brushes, static_cast<size_t>(n[i].leafBrushCount), kAlign2);
        array(c.leafsurfaces, c.numLeafSurfaces, kAlign4);
        array(c.verts, c.vertCount, kAlign4);
        array(c.triIndices, 3ull * c.triCount, kAlign2);
        if (c.triEdgeIsWalkable)
            c.triEdgeIsWalkable = static_cast<unsigned char*>(
                stream(alloc(kAlign1), ((3 * c.triCount + 31) >> 3) & ~3u));
        array(c.borders, c.borderCount, kAlign4);
        if (auto* p = array(c.partitions, c.partitionCount, kAlign4))
            for (unsigned i = 0; i < c.partitionCount; i++) inlineOrRef(p[i].borders, 1, kAlign4);
        array(c.aabbTrees, c.aabbTreeCount, kAlign16);
        array(c.cmodels, c.numSubModels, kAlign4);
        if (auto* b = array(c.brushes, c.numBrushes, 127)) {
            for (unsigned i = 0; i < c.numBrushes; i++) {  // Load_cbrush_t 0x124020
                uint64_t raw = reinterpret_cast<uint64_t>(b[i].sides);
                if (raw == kFollow) {  // one side inline (0x1240d0), with its plane
                    b[i].sides = static_cast<iw4::cbrushside_t*>(stream(alloc(kAlign4), sizeof(iw4::cbrushside_t)));
                    plane1(b[i].sides->plane);
                } else if (raw) {
                    if (lenient_) rawRefs_.push_back(raw);
                    b[i].sides = static_cast<iw4::cbrushside_t*>(offsetToPointer(raw));
                }
                inlineOrRef(b[i].baseAdjacentSide, 1, kAlign1);
            }
        }
        array(c.brushBounds, c.numBrushes, 127);
        array(c.brushContents, c.numBrushes, kAlign4);
        array(c.smodelNodes, c.smodelNodeCount, kAlign4);
        if (c.mapEnts) mapEntsAt_ = rpos_;  // its struct is the next thing read
        assetPtr(c.mapEnts, 19, [&](iw4::MapEnts& m) { mapEnts(m); });  // Load_MapEntsPtr 0x11cd50
        if (lenient_ && (c.dynEntDefList[0] || c.dynEntDefList[1])) {
            // DynEntityDefs reference models/fx/physpresets this port doesn't load.
            stopped_ = "clipMap.dynEntDefList";
            pop();
            return;
        }
        requireNull(c.dynEntDefList[0], "clipMap.dynEntDefList");
        requireNull(c.dynEntDefList[1], "clipMap.dynEntDefList");
        for (int k = 0; k < 2; k++) {
            push(BLOCK_RUNTIME);
            array(c.dynEntPoseList[k], c.dynEntCount[k], kAlign4);
            pop();
        }
        for (int k = 0; k < 2; k++) {
            push(BLOCK_RUNTIME);
            array(c.dynEntClientList[k], c.dynEntCount[k], kAlign4);
            pop();
        }
        for (int k = 0; k < 2; k++) {
            push(BLOCK_RUNTIME);
            array(c.dynEntCollList[k], c.dynEntCount[k], kAlign4);
            pop();
        }
        pop();
    }

    struct Saved {
        int block;
        uint32_t pos;
    };
    const std::vector<uint8_t>& img_;
    SimulatedZone& z_;
    size_t rpos_ = 0;
    uint8_t* base_[BLOCK_COUNT] = {};
    uint32_t pos_[BLOCK_COUNT] = {};
    uint32_t high_[BLOCK_COUNT] = {};
    int cur_ = BLOCK_TEMP;
    std::vector<Saved> stack_;
    // partial loads
    bool lenient_ = false;
    bool anchorPending_ = false;
    uint32_t anchorValue_ = 0;
    std::string stopped_;
    std::vector<uint64_t> rawRefs_;
    size_t mapEntsAt_ = 0;
};

}  // namespace

bool simulateLoad(const std::vector<uint8_t>& zoneImage, SimulatedZone& out, std::string& error) {
    out = SimulatedZone{};
    try {
        Loader(zoneImage, out).run();
        return true;
    } catch (const Fail& e) {
        error = e.what();
        return false;
    } catch (const std::bad_alloc&) {
        error = "zonesim: out of memory (bogus blockSize?)";
        return false;
    }
}

}  // namespace zone

namespace zone {
bool simulatePartial(const std::vector<uint8_t>& zoneImage, const std::vector<PartialAsset>& assets,
                     SimulatedZone& out, std::vector<PartialResult>& results, std::string& error) {
    out = SimulatedZone{};
    results.clear();
    try {
        Loader(zoneImage, out).runPartial(assets, results);
        return true;
    } catch (const Fail& e) {
        error = e.what();
        return false;
    } catch (const std::bad_alloc&) {
        error = "zonesim: out of memory";
        return false;
    }
}
}  // namespace zone
