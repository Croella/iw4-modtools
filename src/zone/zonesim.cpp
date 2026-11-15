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
        for (int b = 0; b < BLOCK_COUNT; b++) z_.blockBase[b] = base_[b];
        if (!stack_.empty()) throw Fail("stream stack not empty at end");
        if (rpos_ != img_.size())
            throw Fail("unconsumed file bytes: read " + std::to_string(rpos_) + " of " + std::to_string(img_.size()));
        for (int b = 0; b < BLOCK_COUNT; b++) z_.blockUsed[b] = high_[b];
    }

    // Like run(), but stops (without failing) at the first asset it can't load.
    void runPrefix(size_t& loaded, std::string& stoppedAt) {
        loaded = 0;
        if (img_.size() < sizeof(iw4::XFile) + sizeof(iw4::XAssetList)) throw Fail("zone image too small");
        iw4::XFile xf;
        std::memcpy(&xf, img_.data(), sizeof(xf));
        rpos_ = sizeof(xf);
        z_.memory = std::make_shared<SimulatedZone::Owned>();
        for (int b = 0; b < BLOCK_COUNT; b++) {
            z_.blockSize[b] = xf.blockSize[b];
            z_.memory->chunks.emplace_back(xf.blockSize[b]);
            base_[b] = static_cast<uint8_t*>(z_.memory->chunks.back().p);
            z_.blockBase[b] = base_[b];
        }
        iw4::XAssetList list;
        readFile(&list, sizeof(list));
        push(BLOCK_VIRTUAL);  // script strings, as in run()
        if (list.stringList.strings) {
            auto* arr = static_cast<uint64_t*>(stream(alloc(kAlign4), 8ull * list.stringList.count));
            for (int i = 0; i < list.stringList.count; i++) {
                if (arr[i] == kFollow) arr[i] = reinterpret_cast<uint64_t>(xstringData());
                else if (arr[i]) arr[i] = 0;  // back-references aren't resolved in prefix mode
                z_.scriptStrings.push_back(arr[i] ? reinterpret_cast<const char*>(arr[i]) : "");
            }
        }
        pop();
        push(BLOCK_VIRTUAL);
        if (list.assets) {
            auto* arr = static_cast<iw4::XAsset*>(stream(alloc(kAlign4), 16ull * list.assetCount));
            for (int i = 0; i < list.assetCount; i++) {
                try {
                    loadXAsset(arr[i]);
                } catch (const Fail& e) {
                    stoppedAt = "asset " + std::to_string(i) + " (type " + std::to_string(arr[i].type) + "): " + e.what();
                    break;
                }
                loaded++;
            }
        }
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
            case 5: materialPtr(a.header.material); break;
            case 0: physPresetPtr(a.header.physPreset); break;
            case 3: xmodelSurfsPtr(a.header.modelSurfs, 0); break;
            case 4: xmodelPtr(a.header.model); break;
            case 2: xanimPartsPtr(a.header.parts); break;
            case 28: {
                auto* w = reinterpret_cast<WeaponCompleteRaw*>(a.header.data);
                weaponPtr(w);
                a.header.data = w;
                break;
            }
            case 42: assetPtr(a.header.addonMapEnts, a.type, [&](iw4::AddonMapEnts& m) { addonMapEnts(m); }); break;
            case 30: {
                uint8_t* f = reinterpret_cast<uint8_t*>(&a.header.data);
                fxPtrAt(f);
                break;
            }
            case 27: {  // Load_LocalizeEntryPtr (0x11cac0) -> 0x11c9d0: struct 16 {value, name}
                uint8_t* f = reinterpret_cast<uint8_t*>(&a.header.data);
                rawAssetPtr(f, 27, 16, [&](uint8_t* s) {
                    push(BLOCK_VIRTUAL);
                    rawXString(s);
                    rawXString(s + 8);
                    pop();
                });
                break;
            }
            case 37: {  // Load_StringTablePtr (0x11f3f0) -> 0x11f280: struct 24 {name, columns, rows,
                        // values: 16 each {string, hash}, align 4}
                uint8_t* f = reinterpret_cast<uint8_t*>(&a.header.data);
                rawAssetPtr(f, 37, 24, [&](uint8_t* s) {
                    push(BLOCK_VIRTUAL);
                    rawXString(s);
                    int32_t cols, rows;
                    std::memcpy(&cols, s + 8, 4);
                    std::memcpy(&rows, s + 12, 4);
                    const size_t cells = static_cast<size_t>(std::max(0, cols)) * static_cast<size_t>(std::max(0, rows));
                    if (rd64(s + 16)) {
                        uint8_t* v = static_cast<uint8_t*>(stream(alloc(kAlign4), 16 * cells));
                        wr64(s + 16, v);
                        for (size_t i = 0; i < cells; i++) rawXString(v + 16 * i);
                    }
                    pop();
                });
                break;
            }
            case 9: techsetPtr(reinterpret_cast<uint8_t*>(&a.header.techniqueSet)); break;
            case 10: imagePtr(a.header.image); break;
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
        pathData(g.path);
        requireNull(g.vehicleTrack.segments, "GameWorldSp.vehicleTrack.segments");
        if (auto* gd = array(g.g_glassData, 1, kAlign4)) {  // Load_G_GlassData (SP 0xdda40)
            requireNull(gd->glassPieces, "GameWorldSp.g_glassData.glassPieces");
            requireNull(gd->glassNames, "GameWorldSp.g_glassData.glassNames");
        }
        pop();
    }
    // Load_PathData (0x11e110): nodes (+ per node pathnode_t 0x1261f0: 5 script strings, links),
    // basenodes in RUNTIME, chain arrays (sized by nodeCount), pathVis, nodeTree (0x126330).
    void pathData(iw4::PathData& p) {
        const size_t n = p.nodeCount;
        if (auto* nodes = array(p.nodes, n, kAlign4)) {
            for (size_t i = 0; i < n; i++) {
                iw4::pathnode_constant_t& c = nodes[i].constant;
                for (uint16_t s : {c.targetname, c.script_linkName, c.script_noteworthy, c.target, c.animscript})
                    if (s > z_.scriptStrings.size()) throw Fail("zonesim: path node script string out of range");
                array(c.Links, c.totalLinkCount, kAlign4);
                for (unsigned l = 0; c.Links && l < c.totalLinkCount; l++)
                    if (c.Links[l].nodeNum >= n) throw Fail("zonesim: path link to a node out of range");
            }
        }
        runtime(p.basenodes, 16 * n, kAlign16);
        array(p.chainNodeForNode, n, kAlign2);
        array(p.nodeForChainNode, n, kAlign2);
        if (p.pathVis && static_cast<size_t>(p.visBytes) * 8 < n * (n ? n - 1 : 0))
            throw Fail("zonesim: pathVis smaller than nodeCount * (nodeCount - 1) bits");
        array(p.pathVis, static_cast<size_t>(p.visBytes), kAlign1);
        if (auto* t = array(p.nodeTree, static_cast<size_t>(p.nodeTreeCount), kAlign4))
            for (int i = 0; i < p.nodeTreeCount; i++) pathTree(t[i], n, t, p.nodeTreeCount);
    }
    void pathTree(iw4::pathnode_tree_t& t, size_t nodeCount, iw4::pathnode_tree_t* base, int count) {
        if (t.axis < 0) {
            if (auto* ids = array(t.u.s.nodes, static_cast<size_t>(t.u.s.nodeCount), kAlign2))
                for (int k = 0; k < t.u.s.nodeCount; k++)
                    if (ids[k] >= nodeCount) throw Fail("zonesim: path tree leaf lists a node out of range");
            return;
        }
        for (auto*& c : t.u.child) {
            uint64_t raw = reinterpret_cast<uint64_t>(c);
            if (!raw) continue;
            if (raw == kFollow) {
                c = static_cast<iw4::pathnode_tree_t*>(stream(alloc(kAlign4), sizeof(iw4::pathnode_tree_t)));
                pathTree(*c, nodeCount, base, count);
            } else {
                c = static_cast<iw4::pathnode_tree_t*>(offsetToPointer(raw));
                if (c < base || c >= base + count) throw Fail("zonesim: path tree child outside the tree array");
            }
        }
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
    // Load_XAnimPartsPtr (0x122d30) -> Load_XAnimParts (0x1229e0): struct 136 (VIRTUAL): name;
    // names (u16 script strings x boneCount[9], align 2); notify (8 each, align 4); deltaPart
    // (24, align 4: trans / quat2 / quat tracks, 0x1223f0); dataByte (a1); dataShort (a2);
    // dataInt (a4); randomDataShort (a2); randomDataByte (a1); randomDataInt (a4); indices
    // (u16 a2 when numframes >= 256, else u8 a1; 0x1228b0).
    void xanimPartsPtr(iw4::XAnimParts*& slot) {
        assetPtr(slot, 2, [&](iw4::XAnimParts& a) {
            uint8_t* s = reinterpret_cast<uint8_t*>(&a);
            const bool wide = a.numframes >= 256;
            push(BLOCK_VIRTUAL);
            xstring(a.name);
            rawArray(s + 56, 2ull * a.boneCount[9], kAlign2);
            rawArray(s + 120, 8ull * a.notifyCount, kAlign4);
            if (rd64(s + 128)) {
                uint8_t* dp = static_cast<uint8_t*>(stream(alloc(kAlign4), 24));
                wr64(s + 128, dp);
                auto indexList = [&](uint16_t size) { stream(alloc(kAlign1), wide ? 2ull * size + 2 : size + 1ull); };
                if (rd64(dp)) {  // XAnimPartTrans (0x122950)
                    uint8_t* t = static_cast<uint8_t*>(stream(alloc(kAlign4), 8));
                    wr64(dp, t);
                    uint16_t size;
                    std::memcpy(&size, t, 2);
                    if (size) {
                        stream(t + 8, 32);  // mins, size, frames ptr
                        indexList(size);
                        if (rd64(t + 32)) {
                            const bool small = t[2] != 0;
                            void* f = stream(alloc(small ? kAlign1 : kAlign4), (small ? 3ull : 6ull) * (size + 1));
                            wr64(t + 32, f);
                        }
                    } else {
                        stream(t + 8, 12);  // frame0
                    }
                }
                for (int q = 0; q < 2; q++) {  // quat2 (0x122530), quat (0x1225d0)
                    if (!rd64(dp + 8 + 8 * q)) continue;
                    uint8_t* h = static_cast<uint8_t*>(stream(alloc(kAlign4), 8));
                    wr64(dp + 8 + 8 * q, h);
                    uint16_t size;
                    std::memcpy(&size, h, 2);
                    const size_t key = q ? 8 : 4;
                    if (size) {
                        stream(h + 8, 8);  // frames ptr
                        indexList(size);
                        if (rd64(h + 8)) wr64(h + 8, stream(alloc(kAlign4), key * (size + 1)));
                    } else {
                        stream(h + 8, key);  // frame0
                    }
                }
            }
            rawArray(s + 64, a.dataByteCount, kAlign1);
            rawArray(s + 72, 2ull * a.dataShortCount, kAlign2);
            rawArray(s + 80, 4ull * a.dataIntCount, kAlign4);
            rawArray(s + 88, 2ull * a.randomDataShortCount, kAlign2);
            rawArray(s + 96, a.randomDataByteCount, kAlign1);
            rawArray(s + 104, 4ull * a.randomDataIntCount, kAlign4);
            rawArray(s + 112, (wide ? 2ull : 1ull) * a.indexCount, wide ? kAlign2 : kAlign1);
            pop();
        });
    }
    // ---- weapons (Load_WeaponCompleteDefPtr 0x120b70) ----
    struct WeaponCompleteRaw { uint8_t b[160]; };
    struct WeaponDefRaw { uint8_t b[2192]; };
    struct FxRaw { uint8_t b[40]; };
    static uint64_t rd(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
    static void wr(uint8_t* p, const void* v) { std::memcpy(p, &v, 8); }
    void xstringAt(uint8_t* field) {
        const char* f = reinterpret_cast<const char*>(rd(field));
        xstring(f);
        wr(field, f);
    }
    // Load_FxEffectDefHandle (0x119000) -> 0x118f30: 40 bytes, VIRTUAL, name, elemDefs (288 each,
    // align 4, count = looping + oneShot + emission at +20/+24/+28) -> 0x1191c0 per element.
    void fxPtrAt(uint8_t* field) {
        FxRaw* slot = reinterpret_cast<FxRaw*>(rd(field));
        assetPtr(slot, 30, [&](FxRaw& fx) {
            push(BLOCK_VIRTUAL);
            xstringAt(fx.b);
            if (rd(fx.b + 32)) {
                int32_t c[3];
                std::memcpy(c, fx.b + 20, 12);
                if (c[0] < 0 || c[1] < 0 || c[2] < 0 || c[0] + c[1] + c[2] > 256) throw Fail("zonesim: FxEffectDef elem counts");
                const int n = c[0] + c[1] + c[2];
                uint8_t* e = static_cast<uint8_t*>(stream(alloc(kAlign4), 288ull * n));
                wr(fx.b + 32, e);
                for (int i = 0; i < n; i++) fxElemDef(e + 288 * i);
            }
            pop();
        });
        wr(field, slot);
    }
    // An effect-def reference inside an element (impact/death/emitted, runner visual): the name
    // as an XString (-1 inline, else a back-reference), resolved by name at load (0x130550).
    void fxNameRefAt(uint8_t* field) { xstringAt(field); }
    // Load_FxElemDef (body of 0x1191c0): velSamples (96 * (velIntervalCount + 1)), visSamples
    // (48 * (visStateIntervalCount + 1)), visuals (0x119460), effectOnImpact/Death/Emitted (+232/
    // 240/248), extended (+272: trail 0x30 for type 3, spark fountain 0x34 for type 6, else 1 byte).
    void fxElemDef(uint8_t* e) {
        const uint8_t type = e[176], visualCount = e[177], velN = e[178], visN = e[179];
        if (type > 12) throw Fail("zonesim: FxElemDef elemType " + std::to_string(type));
        rawArray(e + 184, 96ull * (velN + 1), kAlign4);
        rawArray(e + 192, 48ull * (visN + 1), kAlign4);
        uint8_t* vis = e + 200;  // FxElemDefVisuals
        if (type == 11) {        // decal: markArray of visualCount {material, material}
            if (rd(vis)) {
                uint8_t* m = static_cast<uint8_t*>(stream(alloc(kAlign4), 16ull * visualCount));
                wr(vis, m);
                for (int i = 0; i < 2 * visualCount; i++) materialPtrAt(m + 8 * i);
            }
        } else if (visualCount > 1) {
            if (rd(vis)) {
                uint8_t* a = static_cast<uint8_t*>(stream(alloc(kAlign4), 8ull * visualCount));
                wr(vis, a);
                for (int i = 0; i < visualCount; i++) fxVisualAt(type, a + 8 * i);
            }
        } else {
            fxVisualAt(type, vis);
        }
        fxNameRefAt(e + 232);
        fxNameRefAt(e + 240);
        fxNameRefAt(e + 248);
        uint8_t* ext = e + 272;
        if (rd(ext)) {
            if (type == 3) {
                uint8_t* t = static_cast<uint8_t*>(stream(alloc(kAlign4), 0x30));
                wr(ext, t);
                int32_t verts, inds;
                std::memcpy(&verts, t + 20, 4);
                std::memcpy(&inds, t + 32, 4);
                rawArray(t + 24, 20ull * static_cast<uint32_t>(verts), kAlign4);
                rawArray(t + 40, 2ull * static_cast<uint32_t>(inds), kAlign2);
            } else if (type == 6) {
                wr(ext, stream(alloc(kAlign4), 0x34));
            } else {
                wr(ext, stream(alloc(kAlign1), 1));
            }
        }
    }
    // One FxElemVisuals (0x1196a0): model (7), sound name (10), runner effect name (12), nothing
    // for lights (8, 9), else a material handle.
    void fxVisualAt(uint8_t type, uint8_t* field) {
        switch (type) {
            case 7: xmodelPtrAt(field); break;
            case 8:
            case 9: break;
            case 10:
            case 12: xstringAt(field); break;
            default: materialPtrAt(field); break;
        }
    }
    void xmodelPtrAt(uint8_t* field) {
        iw4::XModel* m = reinterpret_cast<iw4::XModel*>(rd(field));
        xmodelPtr(m);
        wr(field, m);
    }
    void materialPtrAt(uint8_t* field) {
        iw4::Material* m = reinterpret_cast<iw4::Material*>(rd(field));
        materialPtr(m);
        wr(field, m);
    }
    // weapon sound (0x280b30 -> 0x123b50 -> 0x123a40): -1 -> align 4, 8-byte name slot, name.
    void soundAt(uint8_t* field) {
        uint64_t v = rd(field);
        if (!v) return;
        if (v != kFollow) {
            offsetToPointer(v);
            return;
        }
        uint8_t* slot = static_cast<uint8_t*>(stream(alloc(kAlign4), 8));
        wr(field, slot);
        xstringAt(slot);
    }
    void stringArrayAt(uint8_t* field, int n) {
        if (!rd(field)) return;
        if (rd(field) != kFollow) {  // shared with an earlier weapon (stock zones)
            wr(field, offsetToPointer(rd(field)));
            return;
        }
        uint8_t* a = static_cast<uint8_t*>(stream(alloc(kAlign4), 8ull * n));
        wr(field, a);
        for (int i = 0; i < n; i++) xstringAt(a + 8 * i);
    }
    void xmodelArrayAt(uint8_t* field, int n) {
        if (!rd(field)) return;
        uint8_t* a = static_cast<uint8_t*>(stream(alloc(kAlign4), 8ull * n));
        wr(field, a);
        for (int i = 0; i < n; i++) xmodelPtrAt(a + 8 * i);
    }
    void scriptStringsAt(uint8_t* field, int n) {
        if (!rd(field)) return;
        if (rd(field) != kFollow) {  // shared with an earlier weapon (stock zones)
            wr(field, offsetToPointer(rd(field)));
            return;
        }
        uint8_t* a = static_cast<uint8_t*>(stream(alloc(kAlign2), 2ull * n));
        wr(field, a);
        for (int i = 0; i < n; i++) {
            uint16_t idx;
            std::memcpy(&idx, a + 2 * i, 2);
            if (idx && idx > z_.scriptStrings.size()) throw Fail("zonesim: weapon script string out of range");
        }
    }
    void weaponDef(uint8_t* d) {  // 0x120c30 (no push of its own)
        xstringAt(d + 0);
        xmodelArrayAt(d + 8, 16);
        xmodelPtrAt(d + 16);
        stringArrayAt(d + 24, 37);
        stringArrayAt(d + 32, 37);
        xstringAt(d + 40);
        for (int k = 0; k < 4; k++) scriptStringsAt(d + 48 + 8 * k, 16);
        fxPtrAt(d + 112);
        fxPtrAt(d + 120);
        for (int off = 128; off < 504; off += 8) soundAt(d + off);
        if (rd(d + 504)) {
            uint8_t* a = static_cast<uint8_t*>(stream(alloc(kAlign4), 248));
            wr(d + 504, a);
            for (int i = 0; i < 31; i++) soundAt(a + 8 * i);
        }
        for (int off : {512, 520, 528, 536}) fxPtrAt(d + off);
        materialPtrAt(d + 544);
        materialPtrAt(d + 552);
        xmodelArrayAt(d + 736, 16);
        for (int off : {744, 752, 760, 768}) xmodelPtrAt(d + off);
        for (int off : {776, 792, 808}) materialPtrAt(d + off);
        for (int off : {832, 848, 872}) xstringAt(d + off);
        for (int off : {1104, 1112, 1120, 1128}) materialPtrAt(d + off);
        requireNull(reinterpret_cast<void*>(rd(d + 1312)), "WeaponDef.physCollmap");
        xmodelPtrAt(d + 1408);
        fxPtrAt(d + 1424);
        fxPtrAt(d + 1432);
        soundAt(d + 1440);
        soundAt(d + 1448);
        rawArray(d + 1472, 124, kAlign4);
        rawArray(d + 1480, 124, kAlign4);
        fxPtrAt(d + 1488);
        fxPtrAt(d + 1496);
        fxPtrAt(d + 1528);
        soundAt(d + 1536);
        xstringAt(d + 1696);
        requireNull(reinterpret_cast<void*>(rd(d + 1712)), "WeaponDef accuracy graph knots");
        xstringAt(d + 1704);
        requireNull(reinterpret_cast<void*>(rd(d + 1720)), "WeaponDef accuracy graph knots");
        for (int off : {1808, 1816, 1856}) xstringAt(d + off);
        rawArray(d + 1904, 80, kAlign4);
        xstringAt(d + 1912);
        xstringAt(d + 1920);
        requireNull(reinterpret_cast<void*>(rd(d + 1928)), "WeaponDef.tracerType");
        soundAt(d + 1960);
        fxPtrAt(d + 1968);
        xstringAt(d + 1976);
        soundAt(d + 2000);
        for (int k = 0; k < 4; k++) soundAt(d + 2008 + 8 * k);
        for (int k = 0; k < 4; k++) soundAt(d + 2040 + 8 * k);
        soundAt(d + 2072);
        soundAt(d + 2080);
    }
    void weaponPtr(WeaponCompleteRaw*& slot) {
        assetPtr(slot, 28, [&](WeaponCompleteRaw& w) {
            uint8_t* c = w.b;
            push(BLOCK_VIRTUAL);
            xstringAt(c + 0);
            if (rd(c + 8)) {
                if (rd(c + 8) != kFollow) throw Fail("zonesim: WeaponDef back-reference");
                uint8_t* d = static_cast<uint8_t*>(stream(alloc(kAlign4), 2192));
                wr(c + 8, d);
                weaponDef(d);
            }
            xstringAt(c + 16);
            scriptStringsAt(c + 24, 32);
            stringArrayAt(c + 32, 37);
            xstringAt(c + 80);
            materialPtrAt(c + 96);
            materialPtrAt(c + 104);
            requireNull(reinterpret_cast<void*>(rd(c + 136)), "WeaponCompleteDef accuracy graph knots");
            requireNull(reinterpret_cast<void*>(rd(c + 144)), "WeaponCompleteDef accuracy graph knots");
            pop();
        });
    }
    // Load_PhysPresetPtr (0x11e670) -> Load_PhysPreset (0x11e580): struct 56; push VIRTUAL; name;
    // sndAliasPrefix (+32); pop.
    void physPresetPtr(iw4::PhysPreset*& slot) {
        assetPtr(slot, 0, [&](iw4::PhysPreset& p) {
            uint8_t* s = reinterpret_cast<uint8_t*>(&p);
            push(BLOCK_VIRTUAL);
            rawXString(s);
            rawXString(s + 32);
            pop();
        });
    }
    // Load_XModelPtr (0x123700) -> Load_XModel (0x123260), in its exact order (struct 408, VIRTUAL):
    // name; boneNames (u16 script strings, align 2); parentList (numBones - numRootBones, align 1);
    // quats (8 each, align 2); trans (12 each, align 4); partClassification (numBones);
    // baseMat (32 each, align 4); materialHandles (align 4, Load_MaterialHandle each); lodInfo[4]
    // inline, each modelSurfs -> Load_XModelSurfsPtr with the lod's numsurfs; collSurfs (48 each,
    // + collTris 48 each); boneInfo (28 each); physPreset; physCollmap.
    void xmodelPtr(iw4::XModel*& slot) {
        assetPtr(slot, 4, [&](iw4::XModel& m) {
            push(BLOCK_VIRTUAL);
            xstring(m.name);
            const size_t nb = m.numBones, nr = m.numRootBones, nonRoot = nb >= nr ? nb - nr : 0;
            inlineOrRef(m.boneNames, nb, kAlign2);
            inlineOrRef(m.parentList, nonRoot, kAlign1);
            inlineOrRef(m.quats, 4 * nonRoot, kAlign2);
            inlineOrRef(m.trans, 3 * nonRoot, kAlign4);
            inlineOrRef(m.partClassification, nb, kAlign1);
            inlineOrRef(m.baseMat, nb, kAlign4);
            if (m.materialHandles) {
                m.materialHandles = static_cast<iw4::Material**>(stream(alloc(kAlign4), 8ull * m.numsurfs));
                for (int i = 0; i < m.numsurfs; i++) materialPtr(m.materialHandles[i]);
            }
            for (int l = 0; l < 4; l++) xmodelSurfsPtr(m.lodInfo[l].modelSurfs, m.lodInfo[l].numsurfs);
            if (auto* cs = array(m.collSurfs, static_cast<size_t>(std::max(0, m.numCollSurfs)), kAlign4))
                for (int i = 0; i < m.numCollSurfs; i++)
                    array(cs[i].collTris, static_cast<size_t>(std::max(0, cs[i].numCollTris)), kAlign4);
            array(m.boneInfo, nb, kAlign4);
            physPresetPtr(m.physPreset);
            requireNull(m.physCollmap, "XModel.physCollmap");
            pop();
        });
    }
    // Load_XModelSurfsPtr (0x123980) -> Load_XModelSurfs (0x1238c0): struct 48 (VIRTUAL); name;
    // surfs (88 each, align 4; count = the referencing lod's numsurfs) -> Load_XSurface each
    // (0x123bb0): vertInfo.vertsBlend (align 2), verts0 in VERTEX (align 16), vertList (16 each)
    // with each collisionTree (56; nodes 16 each align 16, leafs 2 each align 2), triIndices in
    // INDEX (align 16).
    void xmodelSurfsPtr(iw4::XModelSurfs*& slot, size_t lodSurfs) {
        assetPtr(slot, 3, [&](iw4::XModelSurfs& ms) {
            push(BLOCK_VIRTUAL);
            xstring(ms.name);
            if (ms.surfs) {
                ms.surfs = static_cast<iw4::XSurface*>(stream(alloc(kAlign4), 88ull * lodSurfs));
                for (size_t i = 0; i < lodSurfs; i++) {
                    iw4::XSurface& s = ms.surfs[i];
                    const auto& vc = s.vertInfo.vertCount;
                    inlineOrRef(s.vertInfo.vertsBlend,
                                static_cast<size_t>(vc[0] + 3 * vc[1] + 5 * vc[2] + 7 * vc[3]), kAlign2);
                    push(BLOCK_VERTEX);
                    inlineOrRef(s.verts0, s.vertCount, kAlign16);
                    pop();
                    if (auto* vl = inlineOrRef(s.vertList, s.vertListCount, kAlign4))
                        for (unsigned v = 0; v < s.vertListCount; v++)
                            if (auto* ct = inlineOrRef(vl[v].collisionTree, 1, kAlign4)) {
                                array(ct->nodes, ct->nodeCount, kAlign16);
                                array(ct->leafs, ct->leafCount, kAlign2);
                            }
                    push(BLOCK_INDEX);
                    inlineOrRef(s.triIndices, 3ull * s.triCount, kAlign16);
                    pop();
                }
            }
            pop();
        });
    }
    void addonMapEnts(iw4::AddonMapEnts& m) {  // MapEnts without stages
        push(BLOCK_VIRTUAL);
        xstring(m.name);
        if (m.entityString)
            m.entityString = static_cast<char*>(stream(alloc(kAlign1), static_cast<size_t>(m.numEntityChars)));
        uint8_t* t = reinterpret_cast<uint8_t*>(&m.trigger);
        uint32_t models, hulls, slabs;
        std::memcpy(&models, t + 0, 4);
        std::memcpy(&hulls, t + 16, 4);
        std::memcpy(&slabs, t + 32, 4);
        rawArray(t + 8, 8ull * models, kAlign4);
        rawArray(t + 24, 32ull * hulls, kAlign4);
        rawArray(t + 40, 20ull * slabs, kAlign4);
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

const uint8_t* SimulatedZone::resolve(uint64_t ref) const {
    if (!ref) return nullptr;
    uint64_t v = ref - 1, block = v >> 28, off = v & 0x0FFFFFFF;
    if (block >= 8 || !blockBase[block] || off >= blockUsed[block]) return nullptr;
    return blockBase[block] + off;
}

bool simulatePrefix(const std::vector<uint8_t>& zoneImage, SimulatedZone& out, size_t& loadedAssets,
                    std::string& stoppedAt) {
    out = SimulatedZone{};
    try {
        Loader(zoneImage, out).runPrefix(loadedAssets, stoppedAt);
        return true;
    } catch (const Fail& e) {
        stoppedAt = e.what();
        return false;
    } catch (const std::bad_alloc&) {
        stoppedAt = "zonesim: out of memory";
        return false;
    }
}

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
