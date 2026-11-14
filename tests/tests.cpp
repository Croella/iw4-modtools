// Core tests. Tests that need game data skip themselves if the install is missing.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <vector>
#include <miniz.h>
#include "fastfile.h"
#include "iwi.h"
#include "mapio.h"
#include "zone/iw4.h"
#include "zone/mapimport.h"
#include "zone/zonebuild.h"
#include "zone/zonedump.h"
#include "zone/zonesim.h"
#include "model/imageio.h"
#include "model/xmodelexport.h"
#include "zone/modelbuild.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

// Structure tests compile with the uniform placeholder lighting (fast); testLightingBake covers baking.
static const CompileOptions kNoBake = [] {
    CompileOptions o;
    o.bakeLighting = false;
    return o;
}();

static const char* kGame = "C:/Program Files (x86)/Steam/steamapps/common/Call of Duty Modern Warfare 2";

static void testBox() {
    Brush b = makeBox({-64, -32, 0}, {64, 32, 128}, "caulk");
    CHECK(b.faces.size() == 6);
    CHECK(b.bounds.mins.x == -64 && b.bounds.maxs.z == 128);
    for (const Face& f : b.faces) CHECK(f.winding.size() == 4);
    // Every normal must point away from the center.
    Vec3 c = b.bounds.center();
    for (const Face& f : b.faces) CHECK(dot(f.normal, f.p[0] - c) > 0);
}

static void testResize() {
    Brush b = makeBox({0, 0, 0}, {64, 64, 64}, "caulk");
    CHECK(b.moveFaces(0, 1, 64));
    CHECK(b.bounds.maxs.x == 128 && b.bounds.mins.x == 0);
    CHECK(b.moveFaces(2, -1, -32));
    CHECK(b.bounds.mins.z == -32 && b.bounds.maxs.z == 64);
    AABB before = b.bounds;
    CHECK(!b.moveFaces(0, 1, -128));  // would invert the brush
    CHECK(b.bounds.maxs.x == before.maxs.x);
    b.translate({16, 0, 0});
    CHECK(b.bounds.mins.x == 16);
    CHECK(b.rebuild() && b.bounds.mins.x == 16);
}

static void testSlopedBrush() {
    Brush b = makeBox({0, 0, 0}, {64, 64, 64}, "caulk");
    Face cut;  // plane through the top edges, cutting the box into a wedge
    cut.p[0] = {0, 0, 0};
    cut.p[1] = {0, 64, 0};
    cut.p[2] = {64, 0, 64};
    cut.material = "caulk";
    b.faces.push_back(cut);
    CHECK(b.rebuild());
    CHECK(length(b.faces.back().normal) > 0.99);
    CHECK(b.faces.back().winding.size() == 4);
}

static void testRay() {
    Brush b = makeBox({0, 0, 0}, {64, 64, 64}, "caulk");
    double t = 0;
    int face = -1;
    CHECK(rayBrush(b, {-100, 32, 32}, {1, 0, 0}, t, face));
    CHECK(std::fabs(t - 100) < 1e-6 && b.faces[face].normal.x < -0.5);
    CHECK(!rayBrush(b, {-100, 100, 32}, {1, 0, 0}, t, face));
    CHECK(!rayBrush(b, {100, 32, 32}, {1, 0, 0}, t, face));
}

static void testMapRoundTrip() {
    Document a;
    a.addBrush(makeBox({0, 0, 0}, {64, 64, 64}, "concrete"));
    a.ents[0].brushes[0].attrs.push_back("contents detail;");
    a.ents[0].brushes[0].faces[1].tex = {128, 256, 8, 16, 45};
    a.addPointEntity("mp_tdm_spawn", {32, 16, 8});
    a.ents[0].set("sunlight", "1.5");

    std::string text = writeMapText(a);
    Document b;
    std::string err;
    CHECK(parseMapText(text, b, err));
    CHECK(b.ents.size() == 2);
    CHECK(b.ents[0].brushes.size() == 1);
    const Brush& br = b.ents[0].brushes[0];
    CHECK(br.attrs.size() == 1 && br.attrs[0] == "contents detail;");
    CHECK(br.bounds.maxs.x == 64);
    CHECK(br.faces[0].material == "concrete");
    CHECK(br.faces[1].tex.w == 128 && br.faces[1].tex.rot == 45);
    CHECK(br.faces[0].tail == "lightmap_gray 16384 16384 0 0 0 0");
    CHECK(b.ents[1].classname() == "mp_tdm_spawn");
    Vec3 o;
    CHECK(b.ents[1].origin(o) && o.x == 32);
    CHECK(writeMapText(b) == text);  // second pass must be byte-identical
}

static void testPatchPreserved() {
    const char* text =
        "iwmap 4\n{\n\"classname\" \"worldspawn\"\n{\nmesh\n{\ncaulk\nlightmap_gray\n}\n}\n"
        "// brush 0\n{\n ( 0 0 64 ) ( 0 64 64 ) ( 64 0 64 ) caulk 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0\n"
        " ( 0 0 0 ) ( 64 0 0 ) ( 0 64 0 ) caulk 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0\n"
        " ( 0 0 0 ) ( 0 0 64 ) ( 64 0 0 ) caulk 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0\n"
        " ( 0 64 0 ) ( 64 64 0 ) ( 0 64 64 ) caulk 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0\n"
        " ( 0 0 0 ) ( 0 64 0 ) ( 0 0 64 ) caulk 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0\n"
        " ( 64 0 0 ) ( 64 0 64 ) ( 64 64 0 ) caulk 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0\n"
        "}\n}\n";
    Document d;
    std::string err;
    CHECK(parseMapText(text, d, err));
    if (!err.empty()) std::printf("  parse error: %s\n", err.c_str());
    CHECK(d.ents[0].rawBlocks.size() == 1);
    CHECK(d.ents[0].rawBlocks[0].find("mesh") != std::string::npos);
    CHECK(d.ents[0].brushes.size() == 1);
    CHECK(writeMapText(d).find("mesh") != std::string::npos);
}

static void testBadMapsRejected() {
    Document d;
    std::string err;
    CHECK(!parseMapText("{\n\"classname\" \"worldspawn\"\n}\n", d, err));
    CHECK(!parseMapText("iwmap 4\n{\n\"classname\" \"worldspawn\"\n", d, err));
    CHECK(!parseMapText("iwmap 4\n{\nnot a key value\n}\n", d, err));
    CHECK(!parseMapText("iwmap 4\n{\n{\n ( 0 0 ) bad\n}\n}\n", d, err));
}

static void testDocument() {
    Document d;
    d.pushUndo();
    Sel s = d.addBrush(makeBox({0, 0, 0}, {256, 256, 128}, "caulk"));
    d.select(s, false);
    CHECK(d.selectionBounds().maxs.x == 256);

    d.pushUndo();
    CHECK(d.hollowSelected(16));
    CHECK(d.ents[0].brushes.size() == 6);
    CHECK(d.sel.size() == 6);
    CHECK(d.undo());
    CHECK(d.ents[0].brushes.size() == 1);
    CHECK(d.redo());
    CHECK(d.ents[0].brushes.size() == 6);

    d.pushUndo();
    d.cloneSelected({512, 0, 0});
    CHECK(d.ents[0].brushes.size() == 12);
    CHECK(d.selectionBounds().mins.x == 512);
    d.pushUndo();
    d.deleteSelected();
    CHECK(d.ents[0].brushes.size() == 6);
    CHECK(d.sel.empty());

    Sel p = d.addPointEntity("light", {1, 2, 3});
    d.select(p, false);
    d.translateSelected({10, 0, 0});
    Vec3 o;
    CHECK(d.ents[p.ent].origin(o) && o.x == 11);
    d.deleteSelected();
    CHECK(d.ents.size() == 1);

    d.select({0, 0}, false);
    d.select({0, 1}, true);
    d.selectedToEntity("trigger_multiple");
    CHECK(d.ents.size() == 2 && d.ents[1].brushes.size() == 2 && d.ents[0].brushes.size() == 4);
    CHECK(d.sel.size() == 2 && d.sel[0].ent == 1);
}

static void testHollowRejectsThin() {
    Document d;
    d.select(d.addBrush(makeBox({0, 0, 0}, {16, 256, 256}, "caulk")), false);
    size_t before = d.ents[0].brushes.size();
    CHECK(!d.hollowSelected(16));
    CHECK(d.ents[0].brushes.size() == before);
}

static void testIwiSynthetic() {
    // 4x4 DXT1 with mips: 1x1 (8) + 2x2 (8) + 4x4 (8) after a 32-byte header.
    std::vector<uint8_t> f(32 + 24, 0);
    f[0] = 'I'; f[1] = 'W'; f[2] = 'i'; f[3] = 8; f[8] = 11;
    f[10] = 4; f[12] = 4; f[14] = 1;
    for (int i = 0; i < 8; i++) { f[32 + i] = 1; f[40 + i] = 2; f[48 + i] = 3; }
    IwiImage img;
    std::string err;
    CHECK(parseIwi(f.data(), f.size(), 2048, img, err));
    CHECK(img.levels.size() == 3);
    CHECK(img.levels[0].width == 4 && img.levels[0].data[0] == 3);  // largest level comes from the end
    CHECK(img.levels[2].width == 1 && img.levels[2].data[0] == 1);
    CHECK(parseIwi(f.data(), f.size(), 2, img, err));
    CHECK(img.levels[0].width == 2);
    f[3] = 6;
    CHECK(!parseIwi(f.data(), f.size(), 2048, img, err));
    CHECK(!parseIwi(f.data(), 10, 2048, img, err));
}

static void testRealIwi() {
    std::string path = std::string(kGame) + "/main/iw_00.iwd";
    if (!std::filesystem::exists(path)) { std::puts("skip real IWI test: no game install"); return; }
    mz_zip_archive zip{};
    CHECK(mz_zip_reader_init_file(&zip, path.c_str(), 0));
    int parsed = 0, skipped = 0, tried = 0;
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count && tried < 400; i++) {
        char name[256];
        mz_zip_reader_get_filename(&zip, i, name, sizeof name);
        std::string n = name;
        if (n.size() < 4 || n.substr(n.size() - 4) != ".iwi") continue;
        tried++;
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        CHECK(data != nullptr);
        IwiImage img;
        std::string err;
        if (parseIwi(static_cast<uint8_t*>(data), size, 512, img, err)) {
            parsed++;
            CHECK(img.levels[0].width <= 512 || img.levels.size() == 1);
        } else {
            skipped++;
            if (skipped <= 3) std::printf("  skipped %s: %s\n", name, err.c_str());
        }
        mz_free(data);
    }
    mz_zip_reader_end(&zip);
    std::printf("real IWI: parsed %d, skipped %d of %d\n", parsed, skipped, tried);
    CHECK(parsed * 10 >= tried * 9);
}

static void testRealZone() {
    std::string path = std::string(kGame) + "/zone/english/boneyard.ff";
    if (!std::filesystem::exists(path)) { std::puts("skip real zone test: no game install"); return; }
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(classifyFastfile(file.data(), file.size()) == FastfileKind::Unsigned);
    std::vector<uint8_t> zone;
    std::string err;
    CHECK(inflateFastfile(file, zone, err));
    ZoneHeader h;
    CHECK(readZoneHeader(zone, h));
    CHECK(h.stringCount == 1340 && h.assetCount == 2913);
    std::map<std::string, std::string> mats;
    scanMaterials(zone, mats);
    int world = 0, resolved = 0;
    for (auto& [name, image] : mats) {
        if (name.rfind("wc/", 0) == 0) world++;
        if (!image.empty()) resolved++;
    }
    std::printf("real zone: %zu materials, %d world, %d with colorMap\n", mats.size(), world, resolved);
    CHECK(mats.size() > 700);
    CHECK(mats["wc/ap_chrome"] == "ap_chrome_col");

    std::vector<uint8_t> empty, out;
    CHECK(!inflateFastfile(empty, out, err));
}

static void testFastfileRoundTripSynthetic() {
    // A payload that exercises the zone header validator plus some runs and random-ish bytes.
    std::vector<uint8_t> payload(72, 0);
    uint64_t inlinePtr = ~0ull;
    uint32_t strings = 3, assets = 5;
    std::memcpy(payload.data() + 40, &strings, 4);
    std::memcpy(payload.data() + 48, &inlinePtr, 8);
    std::memcpy(payload.data() + 56, &assets, 4);
    std::memcpy(payload.data() + 64, &inlinePtr, 8);
    for (int i = 0; i < 5000; i++) payload.push_back(static_cast<uint8_t>(i * 2654435761u >> 13));
    for (int i = 0; i < 3000; i++) payload.push_back(0x42);  // a long run compresses well

    Fastfile ff;
    defaultPrefix(ff.prefix);
    ff.prefix[0] = 1;
    ff.prefix[5] = 0xAB;
    ff.zone = payload;

    std::vector<uint8_t> file;
    std::string err;
    CHECK(writeFastfile(ff, file, err));
    CHECK(classifyFastfile(file.data(), file.size()) == FastfileKind::Unsigned);
    CHECK(std::memcmp(file.data(), "IWffu100", 8) == 0);
    CHECK(file.size() < payload.size());  // the run should make it smaller than raw

    Fastfile back;
    CHECK(readFastfile(file, back, err));
    CHECK(back.zone == payload);
    CHECK(std::equal(ff.prefix, ff.prefix + 9, back.prefix));
    ZoneHeader h;
    CHECK(readZoneHeader(back.zone, h) && h.stringCount == 3 && h.assetCount == 5);

    // The editor's inflate path must read our writer's output identically.
    std::vector<uint8_t> viaInflate;
    CHECK(inflateFastfile(file, viaInflate, err) && viaInflate == payload);
}

// Rebuilds the client's own custom zone and the stock boneyard zone, proving the writer is
// faithful against a file the client accepts and against a large real zone.
static void testFastfileRoundTripReal() {
    const char* serenity = "C:/Users/Lurkz/Desktop/IW4Controller-x64 - Copy/assets/serenity_controller_common.ff";
    std::string boneyard = std::string(kGame) + "/zone/english/boneyard.ff";
    for (const std::string& path : {std::string(serenity), boneyard}) {
        if (!std::filesystem::exists(path)) {
            std::printf("skip round-trip (%s): not present\n", path.c_str());
            continue;
        }
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        Fastfile ff;
        std::string err;
        CHECK(readFastfile(file, ff, err));
        std::vector<uint8_t> rebuilt;
        CHECK(writeFastfile(ff, rebuilt, err));
        Fastfile back;
        CHECK(readFastfile(rebuilt, back, err));
        CHECK(back.zone == ff.zone);  // payload identical after read -> write -> read
        CHECK(std::equal(ff.prefix, ff.prefix + 9, back.prefix));
        std::printf("round-trip %s: payload %zu bytes identical, %zu -> %zu on disk\n",
                    std::filesystem::path(path).filename().string().c_str(), ff.zone.size(), file.size(),
                    rebuilt.size());
    }
}

static uint64_t rd64(const std::vector<uint8_t>& v, size_t o) {
    uint64_t x;
    std::memcpy(&x, v.data() + o, 8);
    return x;
}
static uint32_t rd32(const std::vector<uint8_t>& v, size_t o) {
    uint32_t x;
    std::memcpy(&x, v.data() + o, 4);
    return x;
}

static void testEntityString() {
    Document d;
    d.ents[0].set("sunlight", "1.5");  // worldspawn already has classname
    d.addPointEntity("mp_tdm_spawn", {32, 16, 8});
    d.addBrush(makeBox({0, 0, 0}, {64, 64, 64}, "caulk"));  // brushes excluded from the lump
    std::string es = documentToEntityString(d);
    CHECK(es.find("\"classname\" \"worldspawn\"") != std::string::npos);
    CHECK(es.find("\"sunlight\" \"1.5\"") != std::string::npos);
    CHECK(es.find("\"classname\" \"mp_tdm_spawn\"") != std::string::npos);
    CHECK(es.find("\"origin\" \"32 16 8\"") != std::string::npos);
    CHECK(es.find("caulk") == std::string::npos);  // no geometry in the entity string
    // Two entity blocks (worldspawn + spawn), each wrapped in braces.
    int braces = 0;
    for (char c : es) braces += (c == '{');
    CHECK(braces == 2);
}

// A map with more collision brushes than fit in one leaf (int16 cap) must build a leaf-brush-node
// kd-tree instead of erroring. Checks the zone loads and the tree reaches every brush.
static void testLargeClipMapTree() {
    Document d;
    const int side = 200;  // 200 x 200 = 40000 brushes, well over the 32767 single-leaf cap
    for (int i = 0; i < side; i++)
        for (int j = 0; j < side; j++)
            d.addBrush(makeBox({i * 32.0, j * 32.0, 0}, {i * 32.0 + 16, j * 32.0 + 16, 16}, "concrete"));
    d.addPointEntity("mp_dm_spawn", {0, 0, 24});
    CHECK(d.ents[0].brushes.size() == static_cast<size_t>(side) * side);

    std::vector<uint8_t> ff;
    std::string err;
    CHECK(compileMapToFastfile(d, "mp_big", ff, err, MapTarget::MP, kNoBake));
    Fastfile zf;
    zone::SimulatedZone sim;
    CHECK(readFastfile(ff, zf, err) && zone::simulateLoad(zf.zone, sim, err));
    if (!err.empty()) std::printf("  big clipMap simulateLoad: %s\n", err.c_str());
    auto* c = static_cast<const iw4::clipMap_t*>(sim.find(IW4_CLIPMAP_MP));
    CHECK(c && c->numBrushes == side * side);
    if (!c) return;
    CHECK(c->leafbrushNodesCount > 1);  // a real tree, not one flat leaf

    // Walk the tree from the world leaf; every brush must be reachable and no leaf may exceed the cap.
    CHECK(c->numLeafs >= 2);
    std::vector<char> seen(c->numBrushes, 0);
    size_t leaves = 0, maxLeaf = 0;
    bool offsetsOk = true;
    std::function<void(uint32_t, int)> walk = [&](uint32_t ni, int depth) {
        CHECK(ni < c->leafbrushNodesCount);
        if (ni >= c->leafbrushNodesCount || depth > 100) {
            offsetsOk = false;
            return;
        }
        const iw4::cLeafBrushNode_s& n = c->leafbrushNodes[ni];
        if (n.leafBrushCount > 0) {
            leaves++;
            maxLeaf = std::max<size_t>(maxLeaf, n.leafBrushCount);
            for (int k = 0; k < n.leafBrushCount; k++) {
                uint16_t b = n.data.leaf.brushes[k];
                if (b < c->numBrushes) seen[b] = 1;
            }
        } else {
            walk(ni + n.data.children.childOffset[0], depth + 1);
            walk(ni + n.data.children.childOffset[1], depth + 1);
        }
    };
    walk(c->leafs[1].leafBrushNode, 0);
    size_t reached = 0;
    for (char s : seen) reached += s;
    CHECK(offsetsOk);
    CHECK(maxLeaf <= 32767);
    CHECK(reached == c->numBrushes);  // no brush dropped by the tree
    std::printf("big clipMap: %u brushes -> %u nodes (%zu leaves, max %zu/leaf), all reachable\n", c->numBrushes,
                c->leafbrushNodesCount, leaves, maxLeaf);
}

// SP target: maps/<map> paths, clipmap_sp + game_map_sp (0x70-byte GameWorldSp), SP _load script.
static void testZoneCompileSp() {
    Document d;
    d.addPointEntity("info_player_start", {0, 0, 0});
    std::vector<uint8_t> ff;
    std::string err;
    CHECK(compileMapToFastfile(d, "sp_test", ff, err, MapTarget::SP, kNoBake));
    Fastfile zone;
    CHECK(readFastfile(ff, zone, err));
    const std::vector<uint8_t>& img = zone.zone;
    ZoneHeader h;
    CHECK(readZoneHeader(img, h));
    CHECK(h.assetCount == 6);
    size_t arr = 72 + 8 * h.stringCount;
    uint32_t wantTypes[5] = {IW4_COMWORLD, IW4_FXWORLD, IW4_GFXWORLD, IW4_GAMEWORLD_SP, IW4_CLIPMAP_SP};
    for (int i = 0; i < 5; i++) CHECK(rd32(img, arr + 16 * i) == wantTypes[i]);
    zone::SimulatedZone sim;
    CHECK(zone::simulateLoad(img, sim, err));
    if (!err.empty()) std::printf("  simulateLoad (sp): %s\n", err.c_str());
    CHECK(sim.blockUsed[zone::BLOCK_VIRTUAL] == sim.blockSize[zone::BLOCK_VIRTUAL]);
    auto* gw = static_cast<const iw4::GameWorldSp*>(sim.find(IW4_GAMEWORLD_SP));
    CHECK(gw && std::string(gw->name) == "maps/sp_test.d3dbsp" && gw->path.nodeCount == 0);
    // SP dereferences g_glassData unconditionally, so it must be present (and empty).
    CHECK(gw && gw->g_glassData && gw->g_glassData->pieceCount == 0 && gw->g_glassData->glassNameCount == 0);
    auto* cw = static_cast<const iw4::ComWorld*>(sim.find(IW4_COMWORLD));
    CHECK(cw && std::string(cw->name) == "maps/sp_test.d3dbsp");
    CHECK(sim.find(IW4_CLIPMAP_SP) && !sim.find(IW4_CLIPMAP_MP) && !sim.find(IW4_GAMEWORLD_MP));
    // SP reads mapEnts->stages[0] unconditionally: one default stage must be present.
    auto* me = static_cast<const iw4::MapEnts*>(sim.find(IW4_MAPENTS));
    CHECK(me && me->stageCount == 1 && me->stages && me->stages[0].name &&
          std::string(me->stages[0].name) == "default" && me->stages[0].sunPrimaryLightIndex == 1);
    auto* gsc = static_cast<const iw4::RawFile*>(sim.find(IW4_RAWFILE));
    CHECK(gsc && std::string(gsc->name) == "maps/sp_test.gsc");
    if (gsc) {
        std::vector<uint8_t> text(static_cast<size_t>(gsc->len));
        mz_ulong n = static_cast<mz_ulong>(text.size());
        CHECK(mz_uncompress(text.data(), &n, reinterpret_cast<const unsigned char*>(gsc->buffer), static_cast<mz_ulong>(gsc->compressedLen)) == MZ_OK);
        std::string src(text.begin(), text.end());
        CHECK(src == mapScriptSource("sp_test", MapTarget::SP) && src.find("maps\\_load::main();") != std::string::npos);
    }
}

static void testZoneCompile() {
    Document d;
    d.ents[0].set("ambient", "0.2");
    d.addPointEntity("mp_tdm_spawn", {100, 0, 0});
    d.addPointEntity("mp_dm_spawn", {-100, 0, 0});

    std::vector<uint8_t> ff;
    std::string err;
    CHECK(compileMapToFastfile(d, "mp_test", ff, err, MapTarget::MP, kNoBake));
    CHECK(classifyFastfile(ff.data(), ff.size()) == FastfileKind::Unsigned);

    Fastfile zone;
    CHECK(readFastfile(ff, zone, err));
    const std::vector<uint8_t>& img = zone.zone;

    // XFile.size = everything after the 40-byte header.
    CHECK(rd32(img, 0) == img.size() - 40);
    // XAssetList: no script strings (count 0, null pointer, as stock zones do), 4 assets at -1.
    ZoneHeader h;
    CHECK(readZoneHeader(img, h));
    CHECK(h.stringCount == 0 && h.assetCount == 6);
    CHECK(rd64(img, 48) == 0 && rd64(img, 64) == ~0ull);

    // Asset array right after the XAssetList: types in load order, each header = -2. The entity
    // lump is not its own entry: it loads inline through clipMap_t.mapEnts, as in stock zones.
    size_t arr = 72 + 8 * h.stringCount;
    uint32_t wantTypes[5] = {IW4_COMWORLD, IW4_FXWORLD, IW4_GFXWORLD, IW4_GAMEWORLD_MP, IW4_CLIPMAP_MP};
    for (int i = 0; i < 5; i++) {
        CHECK(rd32(img, arr + 16 * i) == wantTypes[i]);
        CHECK(rd64(img, arr + 16 * i + 8) == ~1ull);  // -2 asset header sentinel
    }

    // The entity lump must be present, NUL-terminated inline.
    std::string es = documentToEntityString(d);
    auto it = std::search(img.begin(), img.end(), es.begin(), es.end());
    CHECK(it != img.end());
    if (it != img.end()) CHECK(img[static_cast<size_t>(it - img.begin()) + es.size()] == 0);

    // Load it the way the game does; blockSize must be exact (VIRTUAL) or exact + the linker's
    // 32-byte XAssetList (TEMP), and nothing may overrun.
    zone::SimulatedZone sim;
    CHECK(zone::simulateLoad(img, sim, err));
    if (!err.empty()) std::printf("  simulateLoad: %s\n", err.c_str());
    CHECK(sim.assets.size() == 10);  // 6 + nested MapEnts + lightmap pair + reflection probe
    auto* gsc = static_cast<const iw4::RawFile*>(sim.find(IW4_RAWFILE));
    CHECK(gsc && std::string(gsc->name) == "maps/mp/mp_test.gsc" && gsc->compressedLen > 0);
    if (gsc) {
        std::vector<uint8_t> text(static_cast<size_t>(gsc->len));
        mz_ulong n = static_cast<mz_ulong>(text.size());
        CHECK(mz_uncompress(text.data(), &n, reinterpret_cast<const unsigned char*>(gsc->buffer), static_cast<mz_ulong>(gsc->compressedLen)) == MZ_OK);
        CHECK(std::string(text.begin(), text.end()) == mapScriptSource("mp_test"));
    }
    CHECK(sim.blockUsed[zone::BLOCK_VIRTUAL] == sim.blockSize[zone::BLOCK_VIRTUAL]);
    CHECK(sim.blockUsed[zone::BLOCK_TEMP] + 32 == sim.blockSize[zone::BLOCK_TEMP]);
    auto* me = static_cast<const iw4::MapEnts*>(sim.find(IW4_MAPENTS));
    CHECK(me && me->numEntityChars == static_cast<int>(es.size() + 1) && es == me->entityString);
    auto* cw = static_cast<const iw4::ComWorld*>(sim.find(IW4_COMWORLD));
    CHECK(cw && std::string(cw->name) == "maps/mp/mp_test.d3dbsp" && cw->primaryLightCount == 2);

    CHECK(!compileMapToFastfile(d, "", ff, err));  // empty name rejected
}

static void testZoneWriterModel() {
    using namespace zone;
    // Back-reference encoding matches DB_ConvertOffsetToPointer: block = (p-1)>>28.
    CHECK(offsetPointer(BLOCK_VIRTUAL, 0) == 0x30000001ull);
    CHECK(((offsetPointer(BLOCK_VIRTUAL, 0x1234) - 1) >> 28) == BLOCK_VIRTUAL);
    CHECK(((offsetPointer(BLOCK_VIRTUAL, 0x1234) - 1) & 0x0FFFFFFF) == 0x1234);

    ZoneWriter w;
    w.write("abcd", 4);  // root TEMP: 4
    w.push(BLOCK_VIRTUAL);
    w.u8(1);                          // VIRTUAL 1
    CHECK(w.align(kAlign4) == 4);     // aligned to 4, no file bytes added
    CHECK(w.here() == offsetPointer(BLOCK_VIRTUAL, 4));
    w.push(BLOCK_TEMP);
    w.zeros(100);                     // TEMP 4 -> 104
    uint64_t slot = w.insertPointer();  // VIRTUAL: align 8 -> 8, +8 -> 16
    CHECK(slot == offsetPointer(BLOCK_VIRTUAL, 8));
    w.pop();                          // leaving TEMP rewinds it to 4
    w.push(BLOCK_TEMP);
    w.zeros(10);                      // TEMP 4 -> 14, below the high-water mark
    w.pop();
    w.pop();
    CHECK(w.depth() == 0);
    CHECK(w.blockSize(BLOCK_TEMP) == 104);
    CHECK(w.blockSize(BLOCK_VIRTUAL) == 16);
    CHECK(w.bytes().size() == 4 + 1 + 100 + 10);  // alignment and insert slots are memory-only
}

// Rebuilds stock rawfile zones with the writer and requires an identical image, blockSize
// included. This is ground truth for the container + block model (TEMP rewind, XAssetList in
// TEMP, VIRTUAL sub-data, packed file stream).
static void testRebuildStockRawfileZones() {
    for (const char* zoneName : {"patch_code_pre_gfx", "code_pre_gfx"}) {
        std::string path = std::string(kGame) + "/zone/english/" + zoneName + ".ff";
        if (!std::filesystem::exists(path)) { std::puts("skip stock rawfile zone: no game install"); return; }
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<uint8_t> img;
        std::string err;
        CHECK(inflateFastfile(file, img, err));
        ZoneHeader h;
        CHECK(readZoneHeader(img, h));
        CHECK(h.stringCount == 0);

        // Parse: asset array at 72, then each RawFile {name, compressedLen, len, buffer} + data.
        std::vector<ZoneAsset> assets;
        size_t o = 72 + 16 * h.assetCount;
        bool ok = true;
        for (uint32_t i = 0; i < h.assetCount && ok; i++) {
            ok = rd32(img, 72 + 16 * i) == IW4_RAWFILE && rd64(img, 72 + 16 * i + 8) == ~0ull;
            uint64_t namePtr = rd64(img, o), bufPtr = rd64(img, o + 16);
            int clen = static_cast<int>(rd32(img, o + 8)), len = static_cast<int>(rd32(img, o + 12));
            ok = ok && namePtr == ~0ull && (bufPtr == ~0ull || bufPtr == 0);
            o += 24;
            std::string name(reinterpret_cast<const char*>(img.data() + o));
            o += name.size() + 1;
            std::vector<uint8_t> buf;
            if (bufPtr) {
                size_t n = clen ? static_cast<size_t>(clen) : static_cast<size_t>(len) + 1;
                buf.assign(img.begin() + o, img.begin() + o + n);
                o += n;
            }
            assets.push_back(buildRawFile(name, clen, len, std::move(buf)));
        }
        CHECK(ok);
        CHECK(o == img.size());
        std::vector<uint8_t> rebuilt = buildZoneImage(assets);
        CHECK(rebuilt.size() == img.size());
        CHECK(rebuilt == img);
        zone::SimulatedZone sim;
        CHECK(zone::simulateLoad(img, sim, err));
        CHECK(sim.blockUsed[zone::BLOCK_VIRTUAL] == sim.blockSize[zone::BLOCK_VIRTUAL]);
        CHECK(sim.blockUsed[zone::BLOCK_TEMP] + 32 == sim.blockSize[zone::BLOCK_TEMP]);
        std::printf("stock %s: %u rawfiles rebuilt byte-identical=%d (TEMP %u, VIRTUAL %u)\n", zoneName,
                    h.assetCount, rebuilt == img, rd32(rebuilt, 8), rd32(rebuilt, 8 + 12));
    }
}

static void testClipMapCompile() {
    Document d;
    d.addBrush(makeBox({-256, -256, -16}, {256, 256, 0}, "caulk"));  // floor
    Brush wedge = makeBox({0, 0, 0}, {64, 64, 64}, "caulk");
    Face cut;  // z <= x: a ramp with one non-axial side
    cut.p[0] = {0, 0, 0};
    cut.p[1] = {0, 64, 0};
    cut.p[2] = {64, 0, 64};
    cut.material = "mtl_ramp";
    wedge.faces.push_back(cut);
    CHECK(wedge.rebuild());
    d.addBrush(wedge);
    d.addBrush(makeBox({300, 0, 0}, {316, 64, 64}, "trigger"));  // non-colliding: skipped
    d.addPointEntity("mp_dm_spawn", {0, 0, 8});

    std::vector<uint8_t> ff;
    std::string err;
    CHECK(compileMapToFastfile(d, "mp_clip", ff, err, MapTarget::MP, kNoBake));
    Fastfile zf;
    CHECK(readFastfile(ff, zf, err));
    zone::SimulatedZone sim;
    CHECK(zone::simulateLoad(zf.zone, sim, err));
    if (!err.empty()) std::printf("  simulateLoad: %s\n", err.c_str());
    auto* c = static_cast<const iw4::clipMap_t*>(sim.find(IW4_CLIPMAP_MP));
    CHECK(c != nullptr);
    if (!c) return;

    CHECK(std::string(c->name) == "maps/mp/mp_clip.d3dbsp");
    CHECK(c->numBrushes == 2);
    // Box: all six sides implicit (axial), each face bordered by four others.
    const iw4::cbrush_t& box = c->brushes[0];
    CHECK(box.numsides == 0 && box.sides == nullptr);
    for (int m = 0; m < 2; m++)
        for (int a = 0; a < 3; a++) CHECK(box.edgeCount[m][a] == 4);
    CHECK(box.baseAdjacentSide == c->brushEdges);
    // -X face (side 0): neighbours -Y, -Z, +Y, +Z clockwise from outside, as in boneyard.
    {
        const unsigned char* e = box.baseAdjacentSide + box.firstAdjacentSideOffsets[0][0];
        std::vector<int> got(e, e + 4), want = {2, 4, 3, 5};
        bool rotated = false;
        for (int r = 0; r < 4; r++) {
            std::rotate(want.begin(), want.begin() + 1, want.end());
            rotated |= got == want;
        }
        CHECK(rotated);
    }
    // Wedge: one non-axial side whose plane pointer resolves into the planes array.
    const iw4::cbrush_t& w = c->brushes[1];
    CHECK(w.numsides == 1 && w.sides == c->brushsides);
    CHECK(w.sides->plane >= c->planes && w.sides->plane < c->planes + c->planeCount);
    CHECK(std::fabs(w.sides->plane->normal[0] + 0.7071f) < 1e-3f && std::fabs(w.sides->plane->normal[2] - 0.7071f) < 1e-3f);
    CHECK(w.sides->plane->type == 3);
    CHECK(w.sides->edgeCount == 4);
    CHECK(w.edgeCount[0][0] == 0 && w.edgeCount[1][2] == 0);  // -X and +Z collapse to edges
    CHECK(w.edgeCount[1][0] == 4);                             // +X is a full square
    CHECK(c->materials[w.sides->materialNum].name == std::string("mtl_ramp"));
    CHECK(w.baseAdjacentSide >= c->brushEdges && w.baseAdjacentSide < c->brushEdges + c->numBrushEdges);
    // Bounds/contents.
    CHECK(c->brushBounds[0].midPoint[2] == -8 && c->brushBounds[0].halfSize[0] == 256);
    CHECK(c->brushContents[0] == 1 && c->brushContents[1] == 1);
    // Tree: node 0 -> leaf 1 in front, leaf 1 -> leaf-brush node 1 listing both brushes.
    CHECK(c->numNodes == 1 && c->nodes[0].children[0] == -2 && c->nodes[0].children[1] == -1);
    CHECK(c->nodes[0].plane >= c->planes && c->nodes[0].plane < c->planes + c->planeCount);
    CHECK(c->numLeafs == 2 && c->leafs[1].leafBrushNode == 1 && c->leafs[0].leafBrushNode == 0);
    const iw4::cLeafBrushNode_s& ln = c->leafbrushNodes[1];
    CHECK(ln.leafBrushCount == 2 && ln.data.leaf.brushes == c->leafbrushes);
    CHECK(c->leafbrushes[0] == 0 && c->leafbrushes[1] == 1);
    CHECK(c->numSubModels == 1 && c->cmodels[0].bounds.halfSize[0] == 256);
    // Entity lump reachable from the clipMap, and the same object DB_Add registered.
    CHECK(c->mapEnts == sim.find(IW4_MAPENTS));
    CHECK(c->mapEnts && std::string(c->mapEnts->entityString) == documentToEntityString(d));
    CHECK(sim.blockUsed[zone::BLOCK_VIRTUAL] == sim.blockSize[zone::BLOCK_VIRTUAL]);
    std::printf("clipMap: %u planes, %u brushes, %u sides, %u edge bytes; VIRTUAL %u TEMP %u\n",
                c->planeCount, c->numBrushes, c->numBrushSides, c->numBrushEdges,
                sim.blockSize[zone::BLOCK_VIRTUAL], sim.blockSize[zone::BLOCK_TEMP]);

    // One byte short of VIRTUAL must be caught as an overrun.
    std::vector<uint8_t> bad = zf.zone;
    uint32_t v = rd32(bad, 8 + 4 * zone::BLOCK_VIRTUAL) - 1;
    std::memcpy(bad.data() + 8 + 4 * zone::BLOCK_VIRTUAL, &v, 4);
    zone::SimulatedZone sim2;
    CHECK(!zone::simulateLoad(bad, sim2, err));
    CHECK(err.find("overrun") != std::string::npos);
}

// Baked lighting: a room with a sky roof and a point light, compiled and loader-checked.
static void testLightingBake() {
    Document d;
    d.ents[0].set("sundirection", "-60 45 0");  // high sun, mostly straight down through the roof
    d.ents[0].set("sunlight", "1.2");
    d.ents[0].set("ambient", "0.05");
    d.addBrush(makeBox({-256, -256, -16}, {256, 256, 0}, "wc/me_concrete"));      // floor
    d.addBrush(makeBox({-256, -256, 0}, {-240, 256, 256}, "wc/me_brick"));        // walls
    d.addBrush(makeBox({240, -256, 0}, {256, 256, 256}, "wc/me_brick"));
    d.addBrush(makeBox({-240, -256, 0}, {240, -240, 256}, "wc/me_brick"));
    d.addBrush(makeBox({-240, 240, 0}, {240, 256, 256}, "wc/me_brick"));
    d.addBrush(makeBox({-256, -256, 256}, {256, 256, 272}, "sky/sky_test"));      // roof: sky
    d.addBrush(makeBox({-64, -64, 0}, {64, 64, 128}, "wc/me_brick"));             // a pillar
    Sel L = d.addPointEntity("light", {0, -180, 64});
    d.ents[L.ent].set("radius", "200");
    d.ents[L.ent].set("_color", "1 0.5 0.2");

    light::Environment env = light::readEnvironment(d);
    CHECK(env.hasSun && env.sunDir.z > 0.8 && env.sunColor.x > 0.5);
    light::Settings s;
    s.skySamples = 16;
    light::Bake b;
    std::string err;
    CHECK(light::bake(d, s, b, err));
    if (!err.empty()) std::printf("  bake: %s\n", err.c_str());
    CHECK(b.width > 0 && b.height > 0 && !b.faces.empty());
    CHECK(b.sunVis.size() == static_cast<size_t>(b.width) * b.height * 4);
    // The floor sees sun somewhere (open roof) and is shadowed somewhere (pillar, walls).
    int lit = 0, dark = 0;
    for (uint8_t v : b.sunVis) (v ? lit : dark)++;
    CHECK(lit > 0 && dark > 0);
    // The orange light makes some luxel clearly red-dominant.
    bool warm = false;
    for (size_t i = 0; i + 2 < b.light.size(); i += 3)
        if (b.light[i] > b.light[i + 2] + 0.2f) warm = true;
    CHECK(warm);

    CompileOptions o;
    o.prebaked = &b;
    std::vector<uint8_t> ff;
    CHECK(compileMapToFastfile(d, "mp_lit", ff, err, MapTarget::MP, o));
    Fastfile zf;
    CHECK(readFastfile(ff, zf, err));
    zone::SimulatedZone sim;
    CHECK(zone::simulateLoad(zf.zone, sim, err));
    if (!err.empty()) std::printf("  simulateLoad (lit): %s\n", err.c_str());
    auto* g = static_cast<const iw4::GfxWorld*>(sim.find(IW4_GFXWORLD));
    auto* cw = static_cast<const iw4::ComWorld*>(sim.find(IW4_COMWORLD));
    CHECK(g && cw);
    if (g) {
        const iw4::GfxWorldDraw& dr = g->draw;
        CHECK(dr.lightmaps[0].primary->width == b.width * 2 && dr.lightmaps[0].primary->height == b.height * 2);
        CHECK(dr.lightmaps[0].secondary->width == b.width && dr.lightmaps[0].secondary->height == b.height * 2);
        for (unsigned i = 0; i < g->surfaceCount; i++) {
            const iw4::GfxSurface& su = g->dpvs.surfaces[i];
            const bool skySurf = su.laf.fields.lightmapIndex == 31;  // the sky roof (when the game is installed)
            CHECK(su.laf.fields.primaryLightIndex == (skySurf ? 0 : 1));
        }
        bool inRange = true;
        for (unsigned i = 0; i < dr.vertexCount; i++) {
            const float* lc = dr.vd.vertices[i].lmapCoord;
            if (lc[0] < 0 || lc[0] > 1 || lc[1] < 0 || lc[1] > 1) inRange = false;
        }
        CHECK(inRange);
        CHECK(g->lightGrid.colorCount >= 1);
        bool sunCell = false;
        for (unsigned i = 0; i < g->lightGrid.entryCount; i++)
            if (g->lightGrid.entries[i].primaryLightIndex == 1) sunCell = true;
        CHECK(sunCell);
    }
    if (cw && cw->primaryLightCount == 2)
        CHECK(std::fabs(cw->primaryLights[1].dir[2] - static_cast<float>(env.sunDir.z)) < 1e-4f);
    std::printf("lighting: atlas %dx%d luxels @ %.0f units, %zu faces\n", b.width, b.height, b.luxelSize, b.faces.size());
}

// Sky: the stock sky shader set is copied from trainer.ff, and sky faces become GfxWorld sky
// surfaces drawn with the embedded material (loader-checked). Needs the game install.
static void testSkyCompile() {
    if (!std::filesystem::exists(std::string(kGame) + "/zone/english/trainer.ff")) {
        std::printf("sky: skipped (no game install)\n");
        return;
    }
    sky::Source src;
    std::string err;
    CHECK(sky::load(kGame, "", src, err));
    if (!err.empty()) std::printf("  sky::load: %s\n", err.c_str());
    CHECK(src.valid() && src.vs.code.size() == 400 && src.ps.code.size() == 260 && src.args.size() == 48);
    CHECK(sky::imageForMaterial("sky") == "sky_sp_trainer_ft");
    CHECK(sky::imageForMaterial("wc/sky_mp_boneyard") == "sky_mp_boneyard_ft");

    Document d;
    d.ents[0].set("sky", "sky_mp_boneyard");
    d.addBrush(makeBox({-256, -256, -16}, {256, 256, 0}, "wc/me_concrete"));
    d.addBrush(makeBox({-256, -256, 512}, {256, 256, 528}, "sky"));
    CompileOptions o = kNoBake;
    o.gameDir = kGame;
    std::vector<std::string> warnings;
    o.warnings = &warnings;
    std::vector<uint8_t> ff;
    CHECK(compileMapToFastfile(d, "mp_sky", ff, err, MapTarget::MP, o));
    CHECK(std::none_of(warnings.begin(), warnings.end(), [](const std::string& w) { return w.rfind("sky", 0) == 0; }));
    Fastfile zf;
    CHECK(readFastfile(ff, zf, err));
    zone::SimulatedZone sim;
    CHECK(zone::simulateLoad(zf.zone, sim, err));
    if (!err.empty()) std::printf("  simulateLoad (sky): %s\n", err.c_str());
    auto* g = static_cast<const iw4::GfxWorld*>(sim.find(IW4_GFXWORLD));
    CHECK(g && g->skyCount == 1);
    if (g && g->skyCount == 1) {
        const iw4::GfxSky& s = g->skies[0];
        CHECK(s.skySurfCount >= 1 && s.skyImage && std::string(s.skyImage->name) == "sky_mp_boneyard_ft");
        CHECK(s.skyImage->mapType == 5);
        const iw4::GfxSurface& surf = g->dpvs.surfaces[s.skyStartSurfs[0]];
        CHECK(surf.material && std::string(surf.material->info.name) == gfx::kSkyMaterial);
        CHECK(surf.laf.fields.lightmapIndex == 31);
        if (surf.material) {
            const iw4::Material& m = *surf.material;
            CHECK(m.techniqueSet && m.textureCount == 1 && m.textureTable[0].u.image == s.skyImage);
            CHECK(m.techniqueSet && m.techniqueSet->techniques[9] == m.techniqueSet->techniques[4] &&
                  m.techniqueSet->techniques[36] == m.techniqueSet->techniques[4]);
        }
    }
    // Every embedded asset is loaded (sim counts them): sky material, techset, decl, two shaders.
    int techsets = 0, shaders = 0;
    for (const auto& a : sim.assets) {
        if (a.type == 9) techsets++;
        if (a.type == 6 || a.type == 7) shaders++;
    }
    CHECK(techsets == 1 && shaders == 2);
}

// World materials are embedded from stock definitions (techset by name from common.ff, images
// streamed by name), so compiled maps don't fall back to the unlit default material.
static void testMaterialEmbed() {
    const std::string trainer = std::string(kGame) + "/zone/english/trainer.ff";
    if (!std::filesystem::exists(trainer)) {
        std::printf("materials: skipped (no game install)\n");
        return;
    }
    std::ifstream in(trainer, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Fastfile zf;
    std::string err;
    CHECK(readFastfile(bytes, zf, err));
    auto lib = std::make_shared<matdef::Library>();
    matdef::scan(zf.zone, *lib);
    CHECK(lib->size() > 50);
    const matdef::Def* pick = nullptr;
    for (const auto& [k, d] : *lib)
        if (matdef::techsetFor(d) == "wc_l_sm_b0c0n0s0") { pick = &d; break; }
    CHECK(pick != nullptr);
    if (!pick) return;
    Document d;
    d.addBrush(makeBox({-128, -128, -16}, {128, 128, 0}, pick->name.substr(3)));  // editor names drop "wc/"
    CompileOptions o = kNoBake;
    o.materials = lib;
    std::vector<std::string> warnings;
    o.warnings = &warnings;
    std::vector<uint8_t> ff;
    CHECK(compileMapToFastfile(d, "mp_mat", ff, err, MapTarget::MP, o));
    CHECK(warnings.empty());
    Fastfile out;
    CHECK(readFastfile(ff, out, err));
    zone::SimulatedZone sim;
    CHECK(zone::simulateLoad(out.zone, sim, err));
    if (!err.empty()) std::printf("  simulateLoad (materials): %s\n", err.c_str());
    auto* g = static_cast<const iw4::GfxWorld*>(sim.find(IW4_GFXWORLD));
    CHECK(g && g->surfaceCount >= 1);
    if (g && g->surfaceCount) {
        const iw4::Material* m = g->dpvs.surfaces[0].material;
        CHECK(m && std::string(m->info.name) == pick->name);
        if (m) {
            CHECK(m->techniqueSet && std::string(m->techniqueSet->name) == ",wc_l_sm_b0c0n0s0");
            CHECK(m->textureCount == 3 && m->stateBitsCount == pick->stateBits.size() / 8);
            bool imagesOk = true;
            for (int i = 0; i < m->textureCount; i++) {
                const iw4::GfxImage* im = m->textureTable[i].u.image;
                if (!im || !im->name || im->texture.loadDef == nullptr || im->texture.loadDef->resourceSize != 0) imagesOk = false;
            }
            CHECK(imagesOk);
        }
    }
    std::printf("materials: %zu stock world materials in trainer, embedded %s\n", lib->size(), pick->name.c_str());

    // Effect material: wc/atmos_particles_dust (falloff + uv anim + additive) gets iw4-radiant's
    // embedded technique set, keeps its constants, and its surface is unlit (no lightmap / sun).
    auto dust = lib->find("wc/atmos_particles_dust");
    CHECK(dust != lib->end());
    if (dust == lib->end()) return;
    CHECK(matdef::shadingFor(dust->second) == matdef::Shading::FalloffAdd);
    Document e;
    e.ents[0].set("sundirection", "-60 45 0");
    e.addBrush(makeBox({-256, -256, -16}, {256, 256, 0}, pick->name.substr(3)));
    {
        Brush dustBox = makeBox({0, 0, 0}, {128, 128, 128}, "atmos_particles_dust");
        dustBox.setTwoSided(true);  // a volume seen from inside
        e.addBrush(std::move(dustBox));
    }
    CompileOptions eo;
    eo.materials = lib;
    eo.light.skySamples = 8;
    std::vector<uint8_t> eff;
    CHECK(compileMapToFastfile(e, "mp_dust", eff, err, MapTarget::MP, eo));
    Fastfile ezf;
    CHECK(readFastfile(eff, ezf, err));
    zone::SimulatedZone esim;
    CHECK(zone::simulateLoad(ezf.zone, esim, err));
    if (!err.empty()) std::printf("  simulateLoad (dust): %s\n", err.c_str());
    auto* eg = static_cast<const iw4::GfxWorld*>(esim.find(IW4_GFXWORLD));
    bool sawDust = false;
    for (unsigned i = 0; eg && i < eg->surfaceCount; i++) {
        const iw4::GfxSurface& su = eg->dpvs.surfaces[i];
        if (!su.material || std::string(su.material->info.name) != "wc/atmos_particles_dust") continue;
        sawDust = true;
        const iw4::Material& m = *su.material;
        CHECK(su.laf.fields.lightmapIndex == 31 && su.laf.fields.primaryLightIndex == 0);
        // Additive effect (sort key 29) goes in the lit-trans range, after the opaque surfaces.
        CHECK(i >= eg->dpvs.litTransSurfsBegin && i < eg->dpvs.litTransSurfsEnd);
        CHECK(eg->dpvs.litOpaqueSurfsEnd == eg->dpvs.litTransSurfsBegin);
        CHECK(m.techniqueSet && std::string(m.techniqueSet->name) == "iw4r_falloff_add_ua");
        CHECK(m.techniqueSet && m.techniqueSet->techniques[4] && m.techniqueSet->techniques[4]->passCount == 1);
        // Lit slots reuse the effect technique (stock layout; null there crashes the game's material sort).
        bool litSlots = m.techniqueSet != nullptr;
        for (int t = 9; t <= 36 && litSlots; t++) litSlots = m.techniqueSet->techniques[t] == m.techniqueSet->techniques[4];
        CHECK(litSlots);
        if (m.techniqueSet && m.techniqueSet->techniques[4]) {
            const iw4::MaterialPass& ps = m.techniqueSet->techniques[4]->passArray[0];
            CHECK(ps.vertexShader && ps.pixelShader && ps.vertexDecl && ps.args);
            CHECK(ps.perPrimArgCount + ps.perObjArgCount + ps.stableArgCount == 9);
        }
        CHECK(m.constantCount == 5 && m.textureCount == 1);
        CHECK(su.tris.triCount == 24);  // 6 faces x 2 tris, plus the mirrored copies
    }
    CHECK(sawDust);
    // Effect brushes are nonsolid: only the floor collides.
    auto* ecm = static_cast<const iw4::clipMap_t*>(esim.find(IW4_CLIPMAP_MP));
    CHECK(ecm && ecm->numBrushes == 1);
}

// A custom map script (<map>.gsc next to the .map) replaces the generated one in the zone.
// Weapon pack (SP): the zone also carries level so_w_<map>'s script and an empty add-on lump.
static void testWeaponPackLevel() {
    CHECK(weaponPackLevel("basic") == "so_w_basic");
    Document d;
    d.ents[0].set("weaponpack", "so_demo_so_bridge");
    d.addBrush(makeBox({-64, -64, -16}, {64, 64, 0}, "wc/me_concrete"));
    CompileOptions o = kNoBake;
    o.addonLevel = weaponPackLevel("basic");
    std::vector<uint8_t> ff;
    std::string err;
    CHECK(compileMapToFastfile(d, "basic", ff, err, MapTarget::SP, o));
    Fastfile zf;
    CHECK(readFastfile(ff, zf, err));
    zone::SimulatedZone sim;
    CHECK(zone::simulateLoad(zf.zone, sim, err));
    if (!err.empty()) std::printf("  simulateLoad (pack): %s\n", err.c_str());
    auto* ame = static_cast<const iw4::AddonMapEnts*>(sim.find(IW4_ADDON_MAP_ENTS));
    CHECK(ame && std::string(ame->name) == "maps/so_w_basic.mapents" && ame->numEntityChars == 1);
    bool levelScript = false, mapScript = false;
    for (const auto& a : sim.assets)
        if (a.type == IW4_RAWFILE) {
            std::string n = static_cast<const iw4::RawFile*>(a.header)->name;
            levelScript |= n == "maps/so_w_basic.gsc";
            mapScript |= n == "maps/basic.gsc";
        }
    CHECK(levelScript && mapScript);
}

static void testCustomMapScript() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "iw4r_script_test";
    fs::create_directories(dir);
    const std::string mapPath = (dir / "sp_scripted.map").string();
    CHECK(mapScriptPath(mapPath) == (dir / "sp_scripted.gsc").string());
    const std::string src = "main()\n{\n\tmaps\\_load::main();\n\tlevel.custom = 1;\n}\n";
    { std::ofstream(mapScriptPath(mapPath), std::ios::binary) << src; }
    CompileOptions o = kNoBake;
    CHECK(readMapScript(mapPath, o.mapScript) && o.mapScript == src);
    Document d;
    d.addBrush(makeBox({-64, -64, -16}, {64, 64, 0}, "wc/me_concrete"));
    std::vector<uint8_t> ff;
    std::string err;
    CHECK(compileMapToFastfile(d, "sp_scripted", ff, err, MapTarget::SP, o));
    Fastfile zf;
    CHECK(readFastfile(ff, zf, err));
    zone::SimulatedZone sim;
    CHECK(zone::simulateLoad(zf.zone, sim, err));
    auto* gsc = static_cast<const iw4::RawFile*>(sim.find(IW4_RAWFILE));
    CHECK(gsc && std::string(gsc->name) == "maps/sp_scripted.gsc");
    if (gsc) {
        std::vector<uint8_t> text(static_cast<size_t>(gsc->len));
        mz_ulong n = static_cast<mz_ulong>(text.size());
        CHECK(mz_uncompress(text.data(), &n, reinterpret_cast<const unsigned char*>(gsc->buffer), static_cast<mz_ulong>(gsc->compressedLen)) == MZ_OK);
        CHECK(std::string(text.begin(), text.end()) == src);
    }
    std::string none;
    CHECK(!readMapScript((dir / "missing.map").string(), none) && none.empty());
    fs::remove(mapScriptPath(mapPath));
}


// A cube in xmodel_export form, as the export plugins write it (v6): one bone, 8 verts, 12 tris.
static std::string cubeXModelExport(const char* material, const char* texture) {
    std::string s = "// test cube\nMODEL\nVERSION 6\n\nNUMBONES 1\nBONE 0 -1 \"tag_origin\"\n\nBONE 0\n"
                    "OFFSET 0.000000, 0.000000, 0.000000\nSCALE 1.000000, 1.000000, 1.000000\n"
                    "X 1.000000, 0.000000, 0.000000\nY 0.000000, 1.000000, 0.000000\nZ 0.000000, 0.000000, 1.000000\n\n";
    const float P[8][3] = {{-16, -16, 0}, {16, -16, 0}, {16, 16, 0}, {-16, 16, 0},
                           {-16, -16, 32}, {16, -16, 32}, {16, 16, 32}, {-16, 16, 32}};
    s += "NUMVERTS 8\n";
    char line[160];
    for (int i = 0; i < 8; i++) {
        std::snprintf(line, sizeof line, "VERT %d\nOFFSET %f, %f, %f\nBONES 1\nBONE 0 1.000000\n\n", i, P[i][0], P[i][1], P[i][2]);
        s += line;
    }
    // quads (outward normal) -> 2 tris each
    const int Q[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {2, 3, 7, 6}, {1, 2, 6, 5}, {3, 0, 4, 7}};
    const float N[6][3] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, 1, 0}, {1, 0, 0}, {-1, 0, 0}};
    const float UV[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    s += "NUMFACES 12\n";
    for (int q = 0; q < 6; q++)
        for (int t = 0; t < 2; t++) {
            int c[3] = {0, t ? 2 : 1, t ? 3 : 2};
            s += "TRI 0 0 0 0\n";
            for (int k = 0; k < 3; k++) {
                std::snprintf(line, sizeof line, "VERT %d\nNORMAL %f %f %f\nCOLOR 1.000000 1.000000 1.000000 1.000000\nUV 1 %f %f\n",
                              Q[q][c[k]], N[q][0], N[q][1], N[q][2], UV[c[k]][0], UV[c[k]][1]);
                s += line;
            }
        }
    s += "\nNUMOBJECTS 1\nOBJECT 0 \"cube\"\n\nNUMMATERIALS 1\n";
    s += std::string("MATERIAL 0 \"") + material + "\" \"Lambert\" \"" + texture + "\"\nCOLOR 0.000000 0.000000 0.000000 1.000000\n";
    return s;
}

// A tiny valid PNG (4x4 RGBA, stored zlib) for embedding tests.
static std::vector<uint8_t> tinyPng() {
    std::vector<uint8_t> raw;
    for (int y = 0; y < 4; y++) {
        raw.push_back(0);
        for (int x = 0; x < 4; x++) {
            raw.push_back(uint8_t(x * 60)), raw.push_back(uint8_t(y * 60)), raw.push_back(200), raw.push_back(255);
        }
    }
    mz_ulong cap = mz_compressBound(static_cast<mz_ulong>(raw.size()));
    std::vector<uint8_t> z(cap);
    mz_compress(z.data(), &cap, raw.data(), static_cast<mz_ulong>(raw.size()));
    z.resize(cap);
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    auto chunk = [&](const char* tag, const std::vector<uint8_t>& body) {
        uint32_t n = static_cast<uint32_t>(body.size());
        uint8_t len[4] = {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)};
        png.insert(png.end(), len, len + 4);
        std::vector<uint8_t> tb(tag, tag + 4);
        tb.insert(tb.end(), body.begin(), body.end());
        png.insert(png.end(), tb.begin(), tb.end());
        mz_ulong crc = mz_crc32(MZ_CRC32_INIT, tb.data(), tb.size());
        uint8_t c[4] = {uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)};
        png.insert(png.end(), c, c + 4);
    };
    chunk("IHDR", {0, 0, 0, 4, 0, 0, 0, 4, 8, 6, 0, 0, 0});
    chunk("IDAT", z);
    chunk("IEND", {});
    return png;
}

// zone_source/xmodels/*.xmodel_export -> XModel in the map zone, loader-checked: one bone, one
// rigid surface, a custom material on the stock mc_l_sm_b0c0 template with the texture embedded.
static void testZoneSourceModel() {
    namespace fs = std::filesystem;
    const std::string trainer = std::string(kGame) + "/zone/english/trainer.ff";
    if (!fs::exists(trainer)) {
        std::printf("zone_source models: skipped (no game install)\n");
        return;
    }
    // parser + builder
    xmodelexport::Model src;
    std::string err;
    CHECK(xmodelexport::parse(cubeXModelExport("crate", "c:/art/crate_col.tga"), src, err));
    CHECK(src.bones.size() == 1 && src.verts.size() == 8 && src.faces.size() == 12 && src.materials.size() == 1);
    CHECK(src.materials[0].texture == "c:/art/crate_col.tga");
    modelbuild::Model md;
    CHECK(modelbuild::build(src, "iw4r_test_crate", md, err));
    CHECK(md.surfaces.size() == 1 && md.surfaces[0].tris.size() == 36 && md.radius > 20);

    fs::path dir = fs::temp_directory_path() / "iw4r_zs_test";
    fs::create_directories(dir / "zone_source" / "xmodels");
    fs::create_directories(dir / "zone_source" / "images");
    { std::ofstream(dir / "zone_source" / "xmodels" / "iw4r_test_crate.xmodel_export") << cubeXModelExport("crate", "c:/art/crate_col.png"); }
    { auto png = tinyPng(); std::ofstream(dir / "zone_source" / "images" / "crate_col.png", std::ios::binary).write(reinterpret_cast<const char*>(png.data()), png.size()); }
    imageio::Rgba img;
    CHECK(imageio::load((dir / "zone_source" / "images" / "crate_col.png").string(), img, err) && img.width == 4 && img.pixels[4] == 60);

    std::ifstream in(trainer, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Fastfile tz;
    CHECK(readFastfile(bytes, tz, err));
    auto lib = std::make_shared<matdef::Library>();
    matdef::scan(tz.zone, *lib);

    Document d;
    d.addBrush(makeBox({-128, -128, -16}, {128, 128, 0}, "wc/me_concrete"));
    Sel e = d.addPointEntity("misc_model", {0, 0, 0});
    d.ents[e.ent].set("model", "iw4r_test_crate");
    CompileOptions o = kNoBake;
    o.materials = lib;
    o.zoneSourceDir = (dir / "zone_source").string();
    std::vector<std::string> warnings;
    o.warnings = &warnings;
    std::vector<uint8_t> ff;
    CHECK(compileMapToFastfile(d, "sp_models", ff, err, MapTarget::SP, o));
    for (const std::string& w : warnings) std::printf("  warning: %s\n", w.c_str());
    Fastfile zf;
    CHECK(readFastfile(ff, zf, err));
    zone::SimulatedZone sim;
    CHECK(zone::simulateLoad(zf.zone, sim, err));
    if (!err.empty()) std::printf("  simulateLoad (models): %s\n", err.c_str());
    auto* xm = static_cast<const iw4::XModel*>(sim.find(IW4_XMODEL));
    CHECK(xm != nullptr);
    if (xm) {
        CHECK(std::string(xm->name) == "iw4r_test_crate" && xm->numBones == 1 && xm->numsurfs == 1 && xm->numLods == 1);
        CHECK(xm->boneNames && sim.scriptStrings.size() >= 2 && sim.scriptStrings[xm->boneNames[0]] == "tag_origin");
        CHECK(xm->baseMat && xm->baseMat[0].quat[3] == 1.0f && xm->baseMat[0].transWeight == 2.0f);
        const iw4::XModelLodInfo& lod = xm->lodInfo[0];
        CHECK(lod.modelSurfs && lod.modelSurfs->surfs && lod.numsurfs == 1);
        if (lod.modelSurfs && lod.modelSurfs->surfs) {
            const iw4::XSurface& xs = lod.modelSurfs->surfs[0];
            CHECK(xs.triCount == 12 && xs.vertCount >= 8 && xs.vertListCount == 1 && xs.vertList[0].vertCount == xs.vertCount);
            CHECK(xs.triIndices && xs.verts0);
        }
        const iw4::Material* mt = xm->materialHandles ? xm->materialHandles[0] : nullptr;
        CHECK(mt && std::string(mt->info.name) == "mc/iw4r_test_crate_crate");
        if (mt) {
            CHECK(mt->techniqueSet && std::string(mt->techniqueSet->name) == ",mc_l_sm_b0c0");
            CHECK(mt->textureCount == 1 && mt->textureTable[0].u.image &&
                  std::string(mt->textureTable[0].u.image->name) == "iw4r_iw4r_test_crate_crate_col" &&
                  mt->textureTable[0].u.image->texture.loadDef->resourceSize == 4 * 4 * 4);
        }
    }
    // the misc_model became a script_model in the entity lump
    auto* me = static_cast<const iw4::MapEnts*>(sim.find(IW4_MAPENTS));
    CHECK(me && std::string(me->entityString).find("\"classname\" \"script_model\"") != std::string::npos);
    fs::remove_all(dir);
}

static void testGfxWorldCompile() {
    // Packed normals: byte = v*127 + 127, scale byte 63 (decodes as in stock vertices).
    Vec3 n = gfx::unpackUnitVec(gfx::packUnitVec(normalize(Vec3{0.85, -0.52, 0})));
    CHECK(std::fabs(n.x - 0.85) < 0.02 && std::fabs(n.y + 0.52) < 0.02 && std::fabs(n.z) < 0.02);
    CHECK(gfx::unpackUnitVec(0x3f7f3debu).x > 0.84);  // a stock boneyard vertex normal

    Document d;
    d.addBrush(makeBox({-256, -256, -16}, {256, 256, 0}, "wc/me_concrete"));
    d.addBrush(makeBox({-256, -256, 0}, {-240, 256, 128}, "wc/me_brick"));
    d.addBrush(makeBox({0, 0, 0}, {64, 64, 64}, "caulk"));  // collides, never drawn
    d.addPointEntity("mp_dm_spawn", {0, 0, 8});
    std::vector<uint8_t> ff;
    std::string err;
    CHECK(compileMapToFastfile(d, "mp_gfx", ff, err, MapTarget::MP, kNoBake));
    Fastfile zf;
    CHECK(readFastfile(ff, zf, err));
    zone::SimulatedZone sim;
    CHECK(zone::simulateLoad(zf.zone, sim, err));
    if (!err.empty()) std::printf("  simulateLoad: %s\n", err.c_str());
    auto* g = static_cast<const iw4::GfxWorld*>(sim.find(IW4_GFXWORLD));
    auto* c = static_cast<const iw4::clipMap_t*>(sim.find(IW4_CLIPMAP_MP));
    CHECK(g && c);
    if (!g || !c) return;

    CHECK(std::string(g->name) == "maps/mp/mp_gfx.d3dbsp" && std::string(g->baseName) == "mp_gfx");
    // 12 drawable faces (two boxes), caulk excluded; one surface per material.
    CHECK(g->surfaceCount == 2 && g->materialMemoryCount == 2);
    CHECK(sim.count(5, true) == 2);  // two ','-name material references, no full materials
    const iw4::GfxWorldDraw& dr = g->draw;
    CHECK(dr.vertexCount == 12 * 4 && dr.indexCount == 12 * 2 * 3);
    uint32_t tris = 0;
    for (unsigned i = 0; i < g->surfaceCount; i++) {
        const iw4::GfxSurface& s = g->dpvs.surfaces[i];
        tris += s.tris.triCount;
        CHECK(s.material && s.material->info.name[0] == ',');
        CHECK(s.material == g->materialMemory[i].material);  // back-reference resolved to the same asset
        CHECK(s.tris.firstVertex + s.tris.vertexCount <= dr.vertexCount);
        for (unsigned k = 0; k < 3u * s.tris.triCount; k++)
            CHECK(dr.indices[s.tris.baseIndex + k] < s.tris.vertexCount);
        CHECK(s.laf.fields.lightmapIndex == 0 && s.laf.fields.flags == 1);
    }
    CHECK(tris * 3 == dr.indexCount);
    CHECK(std::string(g->materialMemory[0].material->info.name) == ",wc/me_brick");
    // Every vertex normal is one of the six box directions and points outward.
    for (unsigned i = 0; i < dr.vertexCount; i++) {
        Vec3 vn = gfx::unpackUnitVec(dr.vd.vertices[i].normal.packed);
        CHECK(std::fabs(length(vn) - 1.0) < 0.02);
        CHECK(std::fabs(dr.vd.vertices[i].binormalSign) == 1.0f);
    }
    // One cell, one leaf node, one aabb tree holding every surface.
    CHECK(g->dpvsPlanes.cellCount == 1 && g->nodeCount == 1 && g->dpvsPlanes.nodes[0] == 1);
    const iw4::GfxAabbTree& t = g->aabbTrees[0].aabbTree[0];
    CHECK(t.surfaceCount == 2 && t.startSurfIndex == 0 && t.startSurfIndexNoDecal == 2 && t.childCount == 0);
    CHECK(g->dpvs.sortedSurfIndex[0] == 0 && g->dpvs.sortedSurfIndex[3] == 1);
    CHECK(g->dpvs.litOpaqueSurfsEnd == 2 && g->dpvs.emissiveSurfsBegin == 2);
    // clipMap shares GfxWorld's planes, as in stock.
    CHECK(c->planes == g->dpvsPlanes.planes && c->planeCount == static_cast<unsigned>(g->planeCount));
    CHECK(c->checksum == g->checksum);
    // Lighting: one lightmap pair + one probe, inline, formats as stock.
    CHECK(dr.lightmapCount == 1 && dr.reflectionProbeCount == 1);
    CHECK(dr.lightmaps[0].primary->texture.loadDef->format == 0x32);
    CHECK(dr.lightmaps[0].secondary->texture.loadDef->format == 0x15);
    CHECK(dr.reflectionProbes[0]->mapType == 5 && std::string(dr.reflectionProbes[0]->name) == "*reflection_probe0");
    CHECK(dr.reflectionProbes[0]->texture.loadDef->resourceSize == 6 * 21 * 4);
    // Light grid: decode every row the way stock data decodes; every cell has an entry.
    const iw4::GfxLightGrid& lg = g->lightGrid;
    unsigned rows = lg.maxs[lg.rowAxis] - lg.mins[lg.rowAxis] + 1u, entries = 0;
    for (unsigned r = 0; r < rows; r++) {
        const uint8_t* row = reinterpret_cast<const uint8_t*>(lg.rawRowData) + 4 * lg.rowDataStart[r];
        uint16_t colCount, zCount;
        uint32_t first;
        std::memcpy(&colCount, row + 2, 2);
        std::memcpy(&zCount, row + 6, 2);
        std::memcpy(&first, row + 8, 4);
        CHECK(first == entries);
        unsigned cols = 0;
        for (const uint8_t* run = row + 12; cols < colCount; run += run[1] ? 3 : 2) {
            CHECK(run[1] == zCount && run[2] == 0);
            cols += run[0];
            entries += run[0] * run[1];
        }
        CHECK(cols == colCount);
    }
    CHECK(entries == lg.entryCount && lg.colorCount == 1);
    // World primary lights: {empty, sun}.
    auto* cw = static_cast<const iw4::ComWorld*>(sim.find(IW4_COMWORLD));
    CHECK(cw && cw->primaryLightCount == g->primaryLightCount && cw->primaryLights[1].type == 1);
    for (int b : {zone::BLOCK_VIRTUAL, zone::BLOCK_RUNTIME}) CHECK(sim.blockUsed[b] == sim.blockSize[b]);
    CHECK(sim.blockUsed[zone::BLOCK_TEMP] + 32 == sim.blockSize[zone::BLOCK_TEMP]);
    std::printf("GfxWorld: %u surfaces, %u verts, %u indices, %u lightgrid entries; blocks TEMP %u RUNTIME %u VIRTUAL %u\n",
                g->surfaceCount, dr.vertexCount, dr.indexCount, lg.entryCount, sim.blockSize[0], sim.blockSize[2],
                sim.blockSize[3]);
}

static std::string readText(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void testDumpFastfile() {
    namespace fs = std::filesystem;
    fs::path root = fs::temp_directory_path() / "iw4r_test_dump";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    // A compiled map: everything comes back out (zone, report, script, entity lump).
    Document d;
    d.addBrush(makeBox({-128, -128, -16}, {128, 128, 0}, "wc/me_concrete"));
    d.addPointEntity("mp_dm_spawn", {0, 0, 8});
    std::vector<uint8_t> ff;
    std::string err;
    CHECK(compileMapToFastfile(d, "mp_dump", ff, err, MapTarget::MP, kNoBake));
    fs::path ffPath = root / "mp_dump.ff";
    { std::ofstream(ffPath, std::ios::binary).write(reinterpret_cast<const char*>(ff.data()), ff.size()); }
    zone::DumpResult r;
    CHECK(zone::dumpFastfile(ffPath.string(), (root / "out").string(), r, err));
    CHECK(r.simulated);
    fs::path dir = r.folder;
    CHECK(fs::exists(dir / "report.txt") && fs::exists(dir / "mp_dump.zone"));
    CHECK(readText(dir / "files/maps/mp/mp_dump.gsc") == mapScriptSource("mp_dump"));
    CHECK(readText(dir / "entities.txt") == documentToEntityString(d));
    std::string report = readText(dir / "report.txt");
    CHECK(report.find("gfx_map") != std::string::npos && report.find("loader simulation: ok") != std::string::npos);
    Fastfile back;
    CHECK(readFastfile(ff, back, err) && readText(dir / "mp_dump.zone") == std::string(back.zone.begin(), back.zone.end()));

    // A stock zone: its rawfiles are extracted and decompressed.
    std::string stock = std::string(kGame) + "/zone/english/code_pre_gfx.ff";
    if (fs::exists(stock)) {
        zone::DumpResult s;
        CHECK(zone::dumpFastfile(stock, (root / "out").string(), s, err));
        CHECK(s.simulated);
        std::string csv = readText(fs::path(s.folder) / "files/configure.csv");
        CHECK(csv.size() == 4795);  // RawFile.len in the stock zone
        std::printf("dump: %s; stock code_pre_gfx -> %zu files\n", r.summary.c_str(), s.files.size());
    }
    // Not a fastfile: reported, not a crash.
    fs::path junk = root / "junk.ff";
    { std::ofstream(junk, std::ios::binary) << "not a fastfile"; }
    zone::DumpResult j;
    CHECK(zone::dumpFastfile(junk.string(), (root / "out").string(), j, err));
    CHECK(j.summary == "not an IW fastfile");
    fs::remove_all(root, ec);
}

static void testImportFastfile() {
    namespace fs = std::filesystem;
    fs::path root = fs::temp_directory_path() / "iw4r_test_import";
    std::error_code ec;
    fs::create_directories(root, ec);

    // Round trip: compile a map, import it back. Brushes come back from collision, entities from
    // the lump, the drawn faces as the reference mesh.
    Document d;
    d.addBrush(makeBox({-256, -256, -16}, {256, 256, 0}, "me_concrete"));  // bare name, as in .map files
    Brush wedge = makeBox({0, 0, 0}, {64, 64, 64}, "me_brick");
    Face cut;
    cut.p[0] = {0, 0, 0};
    cut.p[1] = {0, 64, 0};
    cut.p[2] = {64, 0, 64};
    cut.material = "me_brick";
    wedge.faces.push_back(cut);
    CHECK(wedge.rebuild());
    d.addBrush(wedge);
    d.addPointEntity("mp_dm_spawn", {32, 16, 8});
    std::vector<uint8_t> ff;
    std::string err;
    CHECK(compileMapToFastfile(d, "mp_roundtrip", ff, err, MapTarget::MP, kNoBake));
    {
        // The compiled zone references the real asset names.
        Fastfile zf;
        zone::SimulatedZone sim;
        CHECK(readFastfile(ff, zf, err) && zone::simulateLoad(zf.zone, sim, err));
        auto* g = static_cast<const iw4::GfxWorld*>(sim.find(IW4_GFXWORLD));
        CHECK(g && g->materialMemoryCount == 2 && std::string(g->materialMemory[0].material->info.name) == ",wc/me_brick");
    }
    fs::path p = root / "mp_roundtrip.ff";
    { std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(ff.data()), ff.size()); }
    Document back;
    mapimport::ImportStats st;
    CHECK(mapimport::importFastfile(p.string(), back, st, err));
    CHECK(!st.partial && st.brushes == 2 && st.brushesSkipped == 0);
    CHECK(back.ents.size() == 2 && back.ents[0].classname() == "worldspawn" && back.ents[1].classname() == "mp_dm_spawn");
    Vec3 o;
    CHECK(back.ents[1].origin(o) && o.x == 32 && o.y == 16);
    if (back.ents[0].brushes.size() == 2) {
        const Brush& b0 = back.ents[0].brushes[0];
        CHECK(b0.bounds.mins.x == -256 && b0.bounds.maxs.z == 0 && b0.faces.size() == 6);
        const Brush& b1 = back.ents[0].brushes[1];
        CHECK(b1.faces.size() == 5);  // wedge: 4 axial faces + the ramp (the 2 collapsed axial sides are dropped)
        bool ramp = false;
        for (const Face& f : b1.faces) ramp |= std::fabs(f.normal.x + 0.7071) < 1e-3 && std::fabs(f.normal.z - 0.7071) < 1e-3;
        CHECK(ramp);
        CHECK(b1.faces[0].material == "me_brick");  // editor names, no "wc/"
    }
    CHECK(back.refMesh && back.refMesh->batches.size() == 2 && back.refMesh->indices.size() == 3u * st.triangles);
    CHECK(back.refMesh && back.refMesh->batches[0].material == "me_brick");

    // A stock singleplayer map, loaded per asset.
    std::string stock = std::string(kGame) + "/zone/english/boneyard.ff";
    if (fs::exists(stock)) {
        Document b;
        mapimport::ImportStats s;
        CHECK(mapimport::importFastfile(stock, b, s, err));
        CHECK(s.partial && s.brushes == 19144 && s.brushesSkipped == 0);
        CHECK(s.surfaces == 24369 && s.surfacesTextured == 24369);
        CHECK(b.ents.size() == 4832 && b.ents[0].classname() == "worldspawn");
        CHECK(b.refMesh && b.refMesh->materialImages.size() > 250);
        std::printf("import: boneyard %zu brushes, %zu surfaces, %zu triangles, %zu material images\n", s.brushes,
                    s.surfaces, s.triangles, b.refMesh ? b.refMesh->materialImages.size() : 0);
    }
    // Spec Ops overlay zones carry only the mission's entities; geometry comes from the base map.
    std::string overlay = std::string(kGame) + "/zone/english/so_intel_boneyard.ff";
    if (fs::exists(overlay) && fs::exists(stock)) {
        Document ov;
        mapimport::ImportStats os;
        CHECK(mapimport::importFastfile(overlay, ov, os, err));
        CHECK(os.partial && os.brushes == 19144);                  // boneyard's collision
        CHECK(ov.refMesh && os.surfaces == 24369);                  // boneyard's render geometry
        CHECK(ov.ents.size() > 4832 && ov.ents[0].classname() == "worldspawn");  // base + mission entities
        CHECK(os.notes.find("boneyard.ff") != std::string::npos);
        std::printf("import: so_intel_boneyard -> %zu entities on boneyard geometry\n", ov.ents.size());
    }
    // Signed MP zones (retail multiplayer maps) import like any stock zone: the authentication
    // blocks are stripped and the same world assets load.
    std::string signedZone = std::string(kGame) + "/zone/english/mp_highrise.ff";
    if (fs::exists(signedZone)) {
        std::ifstream sf(signedZone, std::ios::binary);
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(sf)), std::istreambuf_iterator<char>());
        CHECK(classifyFastfile(file.data(), file.size()) == FastfileKind::Signed);
        // The decoder recovers the real zone payload: it names the map's d3dbsp asset.
        Fastfile sff;
        CHECK(readFastfile(file, sff, err) && sff.signedZone);
        static const char kBsp[] = "maps/mp/mp_highrise.d3dbsp";
        CHECK(std::search(sff.zone.begin(), sff.zone.end(), kBsp, kBsp + sizeof(kBsp) - 1) != sff.zone.end());

        Document b;
        mapimport::ImportStats s;
        CHECK(mapimport::importFastfile(signedZone, b, s, err));
        CHECK(s.partial && s.brushes > 1000 && s.surfaces > 1000);
        CHECK(!b.ents.empty() && b.ents[0].classname() == "worldspawn");
        std::printf("import: mp_highrise (signed) %zu brushes, %zu surfaces, %zu triangles\n", s.brushes, s.surfaces,
                    s.triangles);

        // Recompiling an imported stock map must pass the loader check. Its brush bounds include a huge
        // sky/caulk shell, which once made the uniform light grid billions of entries (VIRTUAL overflow).
        std::vector<uint8_t> cff;
        Fastfile czf;
        zone::SimulatedZone csim;
        CHECK(compileMapToFastfile(b, "mp_highrise_re", cff, err, MapTarget::MP, kNoBake) && readFastfile(cff, czf, err) &&
              zone::simulateLoad(czf.zone, csim, err));
        if (auto* g = static_cast<const iw4::GfxWorld*>(csim.find(IW4_GFXWORLD))) {
            CHECK(g->lightGrid.entryCount <= (4u << 20));
            std::printf("recompile: mp_highrise -> %zu bytes, %u light grid entries, loader ok\n", cff.size(),
                        g->lightGrid.entryCount);
        } else {
            CHECK(false);
            std::printf("recompile: mp_highrise failed: %s\n", err.c_str());
        }
    }
    fs::remove_all(root, ec);
}

int main() {
    testBox();
    testResize();
    testSlopedBrush();
    testRay();
    testMapRoundTrip();
    testPatchPreserved();
    testBadMapsRejected();
    testDocument();
    testHollowRejectsThin();
    testIwiSynthetic();
    testRealIwi();
    testRealZone();
    testFastfileRoundTripSynthetic();
    testFastfileRoundTripReal();
    testEntityString();
    testZoneCompile();
    testZoneCompileSp();
    testZoneWriterModel();
    testRebuildStockRawfileZones();
    testClipMapCompile();
    testLargeClipMapTree();
    testGfxWorldCompile();
    testLightingBake();
    testCustomMapScript();
    testWeaponPackLevel();
    testZoneSourceModel();
    testSkyCompile();
    testMaterialEmbed();
    testDumpFastfile();
    testImportFastfile();
    std::printf(failures ? "%d check(s) FAILED\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
