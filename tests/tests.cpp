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
#include "patch.h"
#include "worldgeom.h"
#include "zone/zonedump.h"
#include "zone/zonesim.h"
#include "model/imageio.h"
#include "model/xmodelexport.h"
#include "zone/modelbuild.h"
#include "zone/pathnodes.h"
#include "zone/efx.h"
#include "brushshapes.h"
#include "terrain.h"
#include "fxpreview.h"
#include "model/skeleton.h"
#include <map>
#include "zone/clipmap.h"

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

// Path nodes: an 8x8 grid (128 apart) on a floor, split by a wall with a gap at y 256..512.
// Links reach 256 units (2D): 306 grid pairs, 30 of them cross the wall outside the gap.
static void testPathNodes() {
    Document d;
    d.addBrush(makeBox({-512, -512, -16}, {512, 512, 0}, "caulk"));
    d.addBrush(makeBox({-8, -512, 0}, {8, 256, 128}, "caulk"));
    d.addPointEntity("info_player_start", {-300, 0, 16});
    for (int x = -448; x <= 448; x += 128)
        for (int y = -448; y <= 448; y += 128) {
            d.addPointEntity("node_pathnode", {double(x), double(y), 24});
            d.ents.back().set("targetname", "n");
        }
    clip::ClipMapData cm;
    std::string err;
    CHECK(clip::buildClipMapData(d, cm, err));
    pathnodes::Graph g = pathnodes::build(d, cm, nullptr);
    CHECK(g.nodes.size() == 64);
    size_t links = 0;
    for (const auto& l : g.links) links += l.size();
    CHECK(links == 2 * 276);
    CHECK(g.nodes.size() == 64 && g.nodes[0].origin[2] == 0);  // dropped to the floor
    // visibility: across the wall (no), along a column (yes); bit = hi + lo * n
    auto vis = [&](size_t a, size_t b) {
        const size_t lo = std::min(a, b), hi = std::max(a, b), bit = hi + lo * g.nodes.size();
        return (g.vis[bit >> 3] >> (bit & 7)) & 1;
    };
    CHECK(g.vis.size() == (64 * 63 + 7) / 8);
    CHECK(vis(3 * 8 + 0, 4 * 8 + 0) == 0 && vis(0, 7) == 1);
    // every node in exactly one tree leaf, leaves small, children below/above the split
    std::vector<int> seen(64, 0);
    for (const pathnodes::TreeNode& t : g.tree) {
        if (t.axis >= 0) continue;
        CHECK(t.nodes.size() <= 4);
        for (uint16_t i : t.nodes) seen[i]++;
    }
    CHECK(std::all_of(seen.begin(), seen.end(), [](int s) { return s == 1; }));

    std::vector<uint8_t> ff;
    CHECK(compileMapToFastfile(d, "sp_nodes", ff, err, MapTarget::SP, kNoBake));
    Fastfile zf;
    CHECK(readFastfile(ff, zf, err));
    zone::SimulatedZone sim;
    CHECK(zone::simulateLoad(zf.zone, sim, err));
    if (!err.empty()) std::printf("  simulateLoad (nodes): %s\n", err.c_str());
    auto* gw = static_cast<const iw4::GameWorldSp*>(sim.find(IW4_GAMEWORLD_SP));
    CHECK(gw && gw->path.nodeCount == 64 && gw->path.nodes && gw->path.nodeTree && gw->path.pathVis);
    if (gw && gw->path.nodes) {
        CHECK(gw->path.nodes[0].constant.totalLinkCount == g.links[0].size());
        CHECK(gw->path.nodes[0].constant.Links[0].nodeNum == g.links[0][0].first);
        CHECK(gw->path.nodes[0].constant.targetname != 0);  // a script string
        const iw4::pathnode_tree_t* root = gw->path.nodeTree;
        CHECK(root->axis >= 0 && root->u.child[0] >= gw->path.nodeTree && root->u.child[0] < gw->path.nodeTree + gw->path.nodeTreeCount);
    }
    auto* me = static_cast<const iw4::MapEnts*>(sim.find(IW4_MAPENTS));
    CHECK(me && std::string(me->entityString).find("node_pathnode") == std::string::npos);
}

// Brush triggers: trigger_multiple brushes -> MapEnts.trigger ("?N" models, a hull per brush with
// slabs for its slanted faces, relative to the entity origin), as stock maps store them.
static void testBrushTriggers() {
    Document d;
    d.ents[0].brushes.push_back(makeBox({-512, -512, -16}, {512, 512, 0}, "caulk"));
    Entity t;
    t.set("classname", "trigger_multiple");
    t.set("targetname", "zone_a");
    t.brushes.push_back(makeBox({100, 100, 0}, {200, 300, 64}, "trigger"));  // axial: no slabs
    std::vector<Brush> wedge = shapes::make(shapes::Shape::Wedge, {200, 100, 0}, {300, 300, 64}, {}, "trigger");
    CHECK(wedge.size() == 1);
    for (Brush& b : wedge) t.brushes.push_back(b);
    d.ents.push_back(t);
    Entity u;  // a second trigger, with its own origin
    u.set("classname", "trigger_hurt");
    u.set("origin", "0 0 100");
    std::vector<Brush> cyl = shapes::make(shapes::Shape::Cylinder, {-64, -64, 0}, {64, 64, 128}, {}, "trigger");
    for (Brush& b : cyl) u.brushes.push_back(b);
    d.ents.push_back(u);
    Entity bm;  // not a trigger: a brush model "*1"
    bm.set("classname", "script_brushmodel");
    bm.brushes.push_back(makeBox({0, 0, 0}, {8, 8, 8}, "caulk"));
    d.ents.push_back(bm);

    // inside test convention used by the preview: normal . p <= dist
    {
        const Brush& b = t.brushes[0];
        bool in = true;
        for (const Face& f : b.faces) in &= dot(f.normal, Vec3{150, 200, 32}) <= f.dist;
        CHECK(in);
    }
    Document e = d;
    clip::ClipMapData cm;
    std::vector<std::string> w;
    clip::buildTriggers(e, cm, &w);
    CHECK(cm.triggerModels.size() == 2 && cm.triggerHulls.size() == 3 && w.size() == 1);
    CHECK(*e.ents[1].find("model") == "?0" && *e.ents[2].find("model") == "?1");
    CHECK(*e.ents[1].find("origin") == "200 200 32");  // the brushes' centre
    CHECK(*e.ents[2].find("origin") == "0 0 100");     // kept
    CHECK(e.ents[3].find("model") && *e.ents[3].find("model") == "*1" && *e.ents[3].find("origin") == "4 4 4");
    if (cm.triggerHulls.size() == 3) {
        const clip::TriggerHull& h0 = cm.triggerHulls[0];
        CHECK(h0.slabCount == 0 && h0.bounds.midPoint[0] == -50 && h0.bounds.midPoint[1] == 0 && h0.bounds.midPoint[2] == 0 &&
              h0.bounds.halfSize[0] == 50 && h0.bounds.halfSize[1] == 100 && h0.bounds.halfSize[2] == 32);
        CHECK(cm.triggerHulls[1].slabCount == 1);          // the wedge's ramp
        CHECK(cm.triggerHulls[2].slabCount >= 1 && cm.triggerHulls[2].slabCount <= 4);  // octagon: diagonal pairs
        CHECK(cm.triggerHulls[2].bounds.midPoint[2] == -36);  // z 0..128 around origin z 100
        // every slab holds all of its brush's vertices
        bool fits = true;
        const Brush* srcs[3] = {&t.brushes[0], &t.brushes[1], &u.brushes[0]};
        const Vec3 orgs[3] = {{200, 200, 32}, {200, 200, 32}, {0, 0, 100}};
        for (int h = 0; h < 3; h++)
            for (int s = 0; s < cm.triggerHulls[h].slabCount; s++) {
                const clip::TriggerSlab& sl = cm.triggerSlabs[cm.triggerHulls[h].firstSlab + s];
                const Vec3 dir{sl.dir[0], sl.dir[1], sl.dir[2]};
                double lo = 1e9, hi = -1e9;
                for (const Face& f : srcs[h]->faces)
                    for (const Vec3& v : f.winding) lo = std::min(lo, dot(v - orgs[h], dir)), hi = std::max(hi, dot(v - orgs[h], dir));
                fits &= std::fabs(sl.midPoint - (lo + hi) / 2) < 0.01 && std::fabs(sl.halfSize - (hi - lo) / 2) < 0.01;
            }
        CHECK(fits);
    }
    CHECK(cm.triggerModels[0].contents == clip::kTriggerContents && cm.triggerModels[0].hullCount == 2 &&
          cm.triggerModels[1].firstHull == 2);

    // A trigger_grade / trigger_weather (post-pass only) is left out of the zone entirely.
    Entity grade;
    grade.set("classname", "trigger_grade");
    grade.brushes.push_back(makeBox({-300, -300, 0}, {-200, -200, 64}, "trigger"));
    d.ents.push_back(grade);
    grade.set("classname", "trigger_weather");
    d.ents.push_back(grade);
    // whole compile: the loader reads them back
    for (MapTarget target : {MapTarget::MP, MapTarget::SP}) {
        std::vector<uint8_t> ff;
        std::string err;
        CHECK(compileMapToFastfile(d, target == MapTarget::SP ? "sp_trig" : "mp_trig", ff, err, target, kNoBake));
        Fastfile zf;
        zone::SimulatedZone sim;
        CHECK(readFastfile(ff, zf, err) && zone::simulateLoad(zf.zone, sim, err));
        if (!err.empty()) std::printf("  simulateLoad (triggers): %s\n", err.c_str());
        auto* me = static_cast<const iw4::MapEnts*>(sim.find(IW4_MAPENTS));
        CHECK(me && me->trigger.count == 2 && me->trigger.hullCount == 3 && me->trigger.models && me->trigger.hulls && me->trigger.slabs);
        if (me && me->trigger.models && me->trigger.hulls) {
            CHECK(me->trigger.models[1].firstHull == 2 && me->trigger.models[1].hullCount == 1);
            CHECK(me->trigger.hulls[0].bounds.halfSize[1] == 100 && me->trigger.hulls[2].bounds.midPoint[2] == -36);
            CHECK(me->trigger.slabCount == cm.triggerSlabs.size() && me->trigger.slabs[0].halfSize == cm.triggerSlabs[0].halfSize);
            const std::string ents(me->entityString);
            CHECK(ents.find("\"model\" \"?0\"") != std::string::npos && ents.find("\"model\" \"?1\"") != std::string::npos);
            CHECK(ents.find("trigger_grade") == std::string::npos && ents.find("trigger_weather") == std::string::npos);
        }
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
    // Directional lightmap: decode the compiled secondary with the stock normal-mapped shader's
    // formula. Flat (n = 0) gives back the baked light; on the floor under the orange light (at
    // 0 -180 64), a normal tilted towards the light gets more than one tilted away.
    {
        CHECK(b.lightDir.size() == b.light.size() / 3 * 4);
        gfx::GfxWorldData gw;
        CHECK(gfx::buildGfxWorldData(d, gw, err, &b, true, nullptr));
        const int W = b.width, H = b.height;
        CHECK(gw.lightmapSecondaries.size() == 1);
        const std::vector<uint8_t>& sec = gw.lightmapSecondaries[0].pixels;
        CHECK(sec.size() == static_cast<size_t>(W) * H * 2 * 4);
        auto decode = [&](size_t px, double nx, double ny, int k) {
            const uint8_t* top = &sec[px * 4];
            const uint8_t* bot = &sec[(px + static_cast<size_t>(W) * H) * 4];
            const double dx = top[3] / 255.0 * 4.08 - 2.08, dy = bot[3] / 255.0 * 4.0645 - 2.0645;
            const double inv = 1 / std::sqrt(1 + dx * dx + dy * dy), invN = 1 / std::sqrt(1 + nx * nx + ny * ny);
            const double c = std::clamp((dx * nx + dy * ny + 1) * inv * invN, 0.0, 1.0);
            const double v = top[2 - k] / 255.0 * invN + bot[2 - k] / 255.0 * c;
            return v * v;
        };
        double worst = 0;
        size_t directed = 0, checked = 0;
        for (size_t px = 0; px < static_cast<size_t>(W) * H; px++) {
            if (sec[px * 4 + 3] != 130 || sec[(px + static_cast<size_t>(W) * H) * 4 + 3] != 130) directed++;
            for (int k = 0; k < 3; k++) {
                const double e = b.light[px * 3 + k];
                if (e > 3.5) continue;  // past what the format holds
                worst = std::max(worst, std::fabs(decode(px, 0, 0, k) - e) / std::max(e, 0.05));
                checked++;
            }
        }
        std::printf("  directional lightmap: %zu of %zu luxels directed, worst flat error %.3f\n", directed, static_cast<size_t>(W) * H,
                    worst);
        CHECK(directed > 0 && checked > 0 && worst < 0.08);
        // Floor luxel 60 units from the light's foot (toward -y); the floor's tangent frame from its
        // vertices: the light is up and towards +y.
        Face floorTop;
        bool found = false;
        for (const Brush& br : d.ents[0].brushes) {
            Brush bb = br;
            if (!bb.rebuild()) continue;
            for (const Face& f : bb.faces)
                if (f.normal.z > 0.99 && std::fabs(f.dist) < 1e-6 && bb.bounds.maxs.x - bb.bounds.mins.x > 400) floorTop = f, found = true;
        }
        CHECK(found);
        if (found) {
            const light::FaceMap* m = b.find(floorTop);
            CHECK(m != nullptr);
            if (m) {
                float u, v;
                b.lmapCoord(*m, {0, -240 + 4, 0}, u, v);
                const size_t px = static_cast<size_t>(v * H) * W + static_cast<size_t>(u * W);
                // Tangent frame as the compiler writes it.
                const gfx::Vertex* fv = nullptr;
                for (const gfx::Vertex& vx : gw.vertices)
                    if (std::fabs(vx.xyz[2]) < 1e-3 && gfx::unpackUnitVec(vx.normal).z > 0.9) fv = &vx;
                CHECK(fv != nullptr);
                if (fv) {
                    const Vec3 t = gfx::unpackUnitVec(fv->tangent), bn = cross(Vec3{0, 0, 1}, t) * fv->binormalSign;
                    const Vec3 toLight = normalize(Vec3{0, 1, 0});
                    const double tx = dot(toLight, t), ty = dot(toLight, bn);
                    const double towards = decode(px, tx * 0.7, ty * 0.7, 0), away = decode(px, -tx * 0.7, -ty * 0.7, 0);
                    std::printf("  floor near the light: red %.3f flat, %.3f tilted towards it, %.3f away\n", decode(px, 0, 0, 0), towards,
                                away);
                    CHECK(towards > away * 1.15);
                }
            }
        }
    }
    // Light grid: 56 directions (corners first/last), lamp side brighter and redder, sky above.
    {
        const auto& gd = light::gridDirections();
        CHECK(gd[0].x < 0 && gd[0].y < 0 && gd[0].z < 0 && gd[55].x > 0 && gd[55].y > 0 && gd[55].z > 0 && gd[3].x > 0 && gd[12].y > 0);
        auto nearest = [&](const Vec3& want) {
            size_t best = 0;
            for (size_t k = 1; k < 56; k++)
                if (dot(gd[k], want) > dot(gd[best], want)) best = k;
            return best;
        };
        // Beside the lamp (at 0 -180 64), 70 units towards +y; and high in the open middle.
        const std::vector<light::GridSample> gs = light::sampleGrid(b, {{0, -110, 64}, {150, 150, 200}});
        const size_t toLamp = nearest({0, -1, 0}), away = nearest({0, 1, 0}), up = nearest({0, 0, 1}), down = nearest({0, 0, -1});
        std::printf("  light grid: by the lamp red %.3f facing it / %.3f away (blue %.3f); open: up %.3f down %.3f\n", gs[0].dir[toLamp].x,
                    gs[0].dir[away].x, gs[0].dir[toLamp].z, gs[1].dir[up].x, gs[1].dir[down].x);
        CHECK(gs[0].dir[toLamp].x > gs[0].dir[away].x * 1.5 && gs[0].dir[toLamp].x > gs[0].dir[toLamp].z * 1.3);
        CHECK(gs[1].dir[up].z > gs[1].dir[down].z);
        // The compiled palette holds sqrt(light) * 127.5.
        gfx::GfxWorldData gw;
        CHECK(gfx::buildGfxWorldData(d, gw, err, &b, true, nullptr));
        CHECK(!gw.lightGrid.colors.empty());
        bool varied = false;
        for (const auto& c : gw.lightGrid.colors)
            for (size_t k = 3; k < 168; k++)
                if (c[k] != c[k % 3]) varied = true;
        CHECK(varied);  // not one colour copied 56 times
    }
    // The CPU path gives the same bake (sun mask exactly, light up to sampling noise); which one
    // ran first depends on the machine having a D3D11 GPU.
    {
        light::Settings sc = s;
        sc.gpu = !s.gpu;
        light::Bake other;
        CHECK(light::bake(d, sc, other, err));
        std::printf("  bake: %s / %s%s%s\n", b.device.c_str(), other.device.c_str(), b.gpuNote.empty() ? "" : " - GPU not used: ",
                    b.gpuNote.c_str());
        size_t sunDiff = 0;
        for (size_t i = 0; i < b.sunVis.size() && i < other.sunVis.size(); i++) sunDiff += b.sunVis[i] != other.sunVis[i];
        std::printf("  bake: sun mask differs at %zu of %zu samples\n", sunDiff, b.sunVis.size());
        // Rays that graze an edge can go either way in float on the two (different FMA rounding).
        CHECK(other.sunVis.size() == b.sunVis.size() && sunDiff * 1000 <= b.sunVis.size() && other.light.size() == b.light.size());
        double sa = 0, sb = 0, sad = 0;
        for (size_t i = 0; i < b.light.size() && i < other.light.size(); i++)
            sa += b.light[i], sb += other.light[i], sad += std::fabs(b.light[i] - other.light[i]);
        CHECK(sa > 0 && std::fabs(sa - sb) / sa < 0.02 && sad / sa < 0.1);
    }

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
    CHECK(b.gpuNote.find("compile") == std::string::npos);  // a GPU that's there must run the kernels

    // Lightmap pages: a 128-luxel page limit at 8 units/luxel needs several pages (the luxel size
    // stays); the zone gets one primary/secondary pair per page and surfaces on each.
    {
        light::Settings ps = s;
        ps.maxAtlas = 128;
        ps.luxelSize = 8;
        light::Bake mb;
        CHECK(light::bake(d, ps, mb, err));
        std::printf("  pages: %zu at %.0f units/luxel, atlas %dx%d\n", mb.pageY.size(), mb.luxelSize, mb.width, mb.height);
        CHECK(mb.pageY.size() >= 2 && mb.luxelSize == 8 && mb.width == 128);
        CompileOptions mo;
        mo.prebaked = &mb;
        std::vector<uint8_t> mff;
        CHECK(compileMapToFastfile(d, "mp_pages", mff, err, MapTarget::MP, mo));
        Fastfile mz;
        CHECK(readFastfile(mff, mz, err));
        zone::SimulatedZone msim;
        CHECK(zone::simulateLoad(mz.zone, msim, err));
        auto* mg = static_cast<const iw4::GfxWorld*>(msim.find(IW4_GFXWORLD));
        CHECK(mg != nullptr);
        if (mg) {
            const iw4::GfxWorldDraw& dr = mg->draw;
            CHECK(dr.lightmapCount == static_cast<int>(mb.pageY.size()));
            for (int p = 0; p < dr.lightmapCount; p++)
                CHECK(dr.lightmaps[p].primary->width == 256 && dr.lightmaps[p].primary->height == mb.pageH[p] * 2 &&
                      dr.lightmaps[p].secondary->height == mb.pageH[p] * 2);
            std::vector<bool> used(dr.lightmapCount, false);
            for (unsigned i = 0; i < mg->surfaceCount; i++) {
                const unsigned li = mg->dpvs.surfaces[i].laf.fields.lightmapIndex;
                if (li < used.size()) used[li] = true;
            }
            bool all = true;
            for (bool u : used) all = all && u;
            CHECK(all);
            bool inRange = true;
            for (unsigned i = 0; i < dr.vertexCount; i++) {
                const float* lc = dr.vd.vertices[i].lmapCoord;
                if (lc[0] < 0 || lc[0] > 1 || lc[1] < 0 || lc[1] > 1) inRange = false;
            }
            CHECK(inRange);
            // A luxel in the middle of a page-1 face decodes (flat) to its baked light.
            for (const auto& [key, m] : mb.faces) {
                if (m.page != 1 || m.w < 3 || m.h < 3) continue;
                const int x = m.x + m.w / 2, y = m.y + m.h / 2, py = y - mb.pageY[1];
                const uint8_t* sec = dr.lightmaps[1].secondary->texture.loadDef->data;
                const uint8_t* top = sec + (static_cast<size_t>(py) * mb.width + x) * 4;
                const uint8_t* bot = sec + (static_cast<size_t>(py + mb.pageH[1]) * mb.width + x) * 4;
                const double dx = top[3] / 255.0 * 4.08 - 2.08, dy = bot[3] / 255.0 * 4.0645 - 2.0645;
                const double c0 = 1 / std::sqrt(1 + dx * dx + dy * dy), v = top[2] / 255.0 + bot[2] / 255.0 * c0;
                const double e = mb.light[(static_cast<size_t>(y) * mb.width + x) * 3];
                std::printf("  page 1 luxel: decoded red %.3f, baked %.3f\n", v * v, e);
                CHECK(e < 3.5 && std::fabs(v * v - e) <= 0.08 * std::max(e, 0.05));
                break;
            }
        }
    }

    // Reflection probes: two reflection_probe entities -> two baked 64x64 cubes (stock format),
    // surfaces split between them, the open roof (+Z face) brighter than the floor (-Z face).
    {
        Document dr2 = d;
        for (double x : {-150.0, 150.0}) {
            Sel P = dr2.addPointEntity("reflection_probe", {x, 0, 64});
            (void)P;
        }
        CompileOptions ro;
        ro.prebaked = &b;
        std::vector<uint8_t> rff;
        CHECK(compileMapToFastfile(dr2, "mp_probe", rff, err, MapTarget::MP, ro));
        Fastfile rz;
        CHECK(readFastfile(rff, rz, err));
        zone::SimulatedZone rsim;
        CHECK(zone::simulateLoad(rz.zone, rsim, err));
        auto* rg = static_cast<const iw4::GfxWorld*>(rsim.find(IW4_GFXWORLD));
        CHECK(rg != nullptr);
        if (rg) {
            const iw4::GfxWorldDraw& dr = rg->draw;
            CHECK(dr.reflectionProbeCount == 2 && rg->cells[0].reflectionProbeCount == 2);
            CHECK(dr.reflectionProbeOrigins[1].origin[0] == 150);
            const iw4::GfxImage* im = dr.reflectionProbes[0];
            CHECK(im->width == 64 && im->mapType == 5 && im->texture.loadDef->resourceSize == 131064);
            auto faceAvg = [&](int face) {  // mip 0 of a face, BGRA -> mean of r+g+b
                const uint8_t* px = im->texture.loadDef->data + static_cast<size_t>(face) * 5461 * 4;
                double s = 0;
                for (int t = 0; t < 64 * 64; t++) s += px[t * 4] + px[t * 4 + 1] + px[t * 4 + 2];
                return s / (64 * 64 * 3);
            };
            std::printf("  probe 0: up %.1f, down %.1f, +x %.1f\n", faceAvg(4), faceAvg(5), faceAvg(0));
            CHECK(faceAvg(4) > faceAvg(5));
            bool used[2] = {false, false};
            for (unsigned i = 0; i < rg->surfaceCount; i++) {
                const unsigned pi = rg->dpvs.surfaces[i].laf.fields.reflectionProbeIndex;
                if (pi < 2) used[pi] = true;
            }
            CHECK(used[0] && used[1]);
        }
    }

    // Primary spotlight ("primary" "1"): a lamp under the roof aimed down past the pillar's side.
    // Faces it reaches switch to it (its shadow mask in sunVis, the spot left out of the baked
    // light, the sun baked in); the zone carries it as primary light 2 with shadow casters and a
    // region, and the grid cells it reaches point models at it.
    {
        Document dp = d;
        Sel S = dp.addPointEntity("light", {96, 0, 240});
        dp.ents[S.ent].set("radius", "400");
        dp.ents[S.ent].set("intensity", "2");
        dp.ents[S.ent].set("fov_outer", "40");
        dp.ents[S.ent].set("angles", "70 180 0");  // down and towards -x: past the pillar
        dp.ents[S.ent].set("primary", "1");
        light::Bake pb;
        CHECK(light::bake(dp, s, pb, err));
        CHECK(pb.primaries.size() == 1);
        int spotFaces = 0, sunFaces = 0;
        for (const auto& [key, m] : pb.faces) (m.primary == 0 ? spotFaces : sunFaces)++;
        std::printf("  primary spot: %d face(s) on the spot, %d on the sun\n", spotFaces, sunFaces);
        CHECK(spotFaces > 0 && sunFaces > 0);
        // Floor on the far (-x) side of the pillar: in the spot's cone, in the pillar's shadow from it.
        Face floorTop;
        for (const Brush& br : dp.ents[0].brushes) {
            Brush bb = br;
            if (!bb.rebuild()) continue;
            for (const Face& f : bb.faces)
                if (f.normal.z > 0.99 && std::fabs(f.dist) < 1e-6 && bb.bounds.maxs.x - bb.bounds.mins.x > 400) floorTop = f;
        }
        const light::FaceMap* fm = pb.find(floorTop);
        CHECK(fm && fm->primary == 0);
        if (fm) {
            auto mask = [&](double x, double y) {
                float u, v;
                pb.lmapCoord(*fm, {x, y, 0}, u, v);
                return pb.sunVis[static_cast<size_t>(v * pb.height * 2) * (pb.width * 2) + static_cast<size_t>(u * pb.width * 2)];
            };
            const int shadowed = mask(-80, 0), lit = mask(-30, 180);
            std::printf("  spot mask on the floor: behind the pillar %d, beside it %d\n", shadowed, lit);
            CHECK(shadowed == 0 && lit == 255);
        }
        CompileOptions po;
        po.prebaked = &pb;
        std::vector<uint8_t> pff;
        CHECK(compileMapToFastfile(dp, "mp_spot", pff, err, MapTarget::MP, po));
        Fastfile pz;
        CHECK(readFastfile(pff, pz, err));
        zone::SimulatedZone psim;
        CHECK(zone::simulateLoad(pz.zone, psim, err));
        if (!err.empty()) std::printf("  simulateLoad (spot): %s\n", err.c_str());
        auto* pg = static_cast<const iw4::GfxWorld*>(psim.find(IW4_GFXWORLD));
        auto* pc = static_cast<const iw4::ComWorld*>(psim.find(IW4_COMWORLD));
        CHECK(pg && pc);
        if (pg && pc) {
            CHECK(pg->primaryLightCount == 3 && pc->primaryLightCount == 3);
            const iw4::ComPrimaryLight& L = pc->primaryLights[2];
            CHECK(L.type == 2 && L.canUseShadowMap == 1 && L.radius == 400 && L.dir[2] > 0.9f && L.origin[2] == 240);
            CHECK(L.defName && std::string(L.defName) == "light_point_linear");
            int spotSurfs = 0;
            for (unsigned i = 0; i < pg->surfaceCount; i++) spotSurfs += pg->dpvs.surfaces[i].laf.fields.primaryLightIndex == 2;
            CHECK(spotSurfs > 0);
            CHECK(pg->shadowGeom[2].surfaceCount > 0 && pg->shadowGeom[0].surfaceCount == 0);
            CHECK(pg->lightRegion[2].hullCount == 1 && pg->lightRegion[1].hullCount == 0);
            const iw4::GfxLightRegionHull& h = pg->lightRegion[2].hulls[0];
            CHECK(h.kdopMidPoint[2] - h.kdopHalfSize[2] < -300 && h.kdopMidPoint[2] + h.kdopHalfSize[2] > 0);  // reaches down
            bool spotCell = false;
            for (unsigned i = 0; i < pg->lightGrid.entryCount; i++)
                if (pg->lightGrid.entries[i].primaryLightIndex == 2) spotCell = true;
            CHECK(spotCell);
            std::printf("  zone: %d spot surface(s), %u shadow caster(s)\n", spotSurfs, pg->shadowGeom[2].surfaceCount);
        }
    }
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

// Effects: an .efx with the values of BO1's raw/fx/muzzleflashes/saw_flshview_c.efx must compile to
// what MW2's common.ff carries for muzzleflashes/saw_flshview_c (numbers from iw4r_zone fxprobe).
static const char* kTestEfx = R"(iwfx 2

{
	name "smoke 2";
	editorFlags useRandVel0 useBackCompatVel;
	flags spawnRelative spawnOffsetNone runRelToSpawn;
	spawnRange 0 0;
	spawnLooping 200 0;
	spawnOneShot 1 0;
	spawnDelayMsec 0 0;
	lifeSpanMsec 500 0;
	spawnOrgX 5 0;
	initialRot 0 360;
	gravity 0 0;
	elasticity 0 0;
	atlasBehavior startRandom;
	atlasIndex 0;
	atlasFps 45;
	atlasLoopCount 0;
	velGraph0X 10 { { 0 0.5 1 0.5 } { 0 0.5 1 0.5 } };
	velGraph0Y 0 { { 0 0.5 1 0.5 } { 0 -0.5 1 -0.5 } };
	velGraph0Z 0 { { 0 0.5 1 0.5 } { 0 -0.5 1 -0.5 } };
	velGraph1X 0 { { 0 0 1 0 } { 0 0 1 0 } };
	velGraph1Y 0 { { 0 0 1 0 } { 0 0 1 0 } };
	velGraph1Z 0 { { 0 0 1 0 } { 0 0 1 0 } };
	rotGraph 10 { { 0 0.351852 1 0.5 } { 0 0 1 0 } };
	sizeGraph0 50 { { 0 0.194805 1 1 } { 0 1 1 0 } };
	sizeGraph1 0 { { 0 1 1 0 } { 0 1 1 0 } };
	scaleGraph 0 { { 0 1 1 0 } { 0 1 1 0 } };
	colorGraph 1 { { 0 1 1 1  1 0.72549 0.72549 0.72549 } { 0 1 1 1  1 0 0 0 } };
	alphaGraph 1 { { 0 0.157895 1 0 } { 0 1 1 0 } };
	lightingFrac 0.25;
	collOffset 0 0 0;
	collRadius 0;
	fxOnImpact "";
	fxOnDeath "";
	sortOrder 0;
	emission "";
	emitDist 0 0;
	emitDistVariance 0 0;
	billboardSprite
	{
		"gfx_smk_white_atlas"
	};
}
{
	name "light";
	editorFlags modColorByAlpha useBackCompatVel;
	flags spawnRelative spawnOffsetNone runRelToSpawn;
	spawnOneShot 1 0;
	lifeSpanMsec 200 0;
	spawnOrgX 5 5;
	atlasBehavior startFixed;
	velGraph0X 0 { { 0 0 1 0 } { 0 0 1 0 } };
	sizeGraph0 200 { { 0 1 1 1 } { 0 1 1 0 } };
	scaleGraph 0 { { 0 1 1 0 } { 0 1 1 0 } };
	colorGraph 1 { { 0 1 1 0.501961  0.18314 1 1 0.501961  0.22093 1 0.501961 0.25098  1 0 0 0 } { 0 1 1 1  1 0 0 0 } };
	alphaGraph 1 { { 0 0.397129 1 0.373206 } { 0 1 1 0 } };
	lightingFrac 0;
	sortOrder 15;
	light;
}
)";

static std::vector<uint8_t> tinyPng();
static void testEffects() {
    namespace fs = std::filesystem;
    efx::Effect fx;
    std::string err;
    CHECK(efx::parse(kTestEfx, fx, err));
    CHECK(fx.version == 2 && fx.elems.size() == 2 && fx.elems[0].visuals.size() == 1 && fx.elems[1].visualType == "light");
    auto lookup = [](const std::string& n) {
        efx::MaterialInfo m;
        if (n == "gfx_smk_white_atlas") m.known = true, m.rows = 4, m.cols = 8;
        return m;
    };
    efx::Compiled c;
    std::vector<std::string> warnings;
    CHECK(efx::compile(fx, "muzzleflashes/saw_flshview_c", lookup, c, warnings, err));
    CHECK(c.elems.size() == 2 && c.countOneShot == 2 && c.countLooping == 0 && c.flags == 1);
    {  // a forever-looping element (spawnLooping count 0): INT_MAX count and looping life, as stock fire/firelp_med_pm
        std::ifstream in("maps/zm_test/zone_source/fx/env/nature/fx_fireflies.efx");
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        efx::Effect ff;
        efx::Compiled fc;
        std::vector<std::string> fw;
        const bool ok = efx::parse(text, ff, err) &&
                        efx::compile(ff, "env/nature/fx_fireflies", [](const std::string&) { efx::MaterialInfo m; m.known = true; return m; },
                                     fc, fw, err);
        CHECK(ok && fc.countLooping == 1 && fc.msecLoopingLife == 0x7FFFFFFF);
        if (ok && !fc.elems.empty()) {
            int32_t interval, count;
            std::memcpy(&interval, fc.elems[0].def.data() + 4, 4);
            std::memcpy(&count, fc.elems[0].def.data() + 8, 4);
            CHECK(interval == 175 && count == 0x7FFFFFFF);
        }
    }
    auto f32 =[](const uint8_t* p) { float v; std::memcpy(&v, p, 4); return v; };
    auto i32 = [](const uint8_t* p) { int32_t v; std::memcpy(&v, p, 4); return v; };
    auto near = [](float a, float b) { return std::fabs(a - b) <= 1e-5f + 1e-4f * std::fabs(b); };
    if (c.elems.size() == 2) {
        const efx::ElemOut& s = c.elems[0];
        const uint8_t* d = s.def.data();
        CHECK(static_cast<uint32_t>(i32(d)) == 0x01000042u);  // local velocity, runRelToSpawn, spawnRelative
        CHECK(i32(d + 4) == 1 && i32(d + 8) == 0 && i32(d + 48) == 500 && near(f32(d + 56), 5));
        CHECK(near(f32(d + 144), 0) && near(f32(d + 148), 6.28319f));                       // initialRot 0..360 deg
        CHECK(d[168] == 1 && d[170] == 45 && d[171] == 1 && d[172] == 3 && d[173] == 2 && d[174] == 32);  // atlas 4x8
        CHECK(d[176] == 0 && d[177] == 1 && d[178] == 1 && d[179] == 1 && d[280] == 0 && d[281] == 63);
        const uint8_t* v0 = s.velSamples.data();
        CHECK(s.velSamples.size() == 192 && near(f32(v0), 0.005f) && near(f32(v0 + 96 + 24), 0.005f));  // v, then totalDelta
        const uint8_t* vs = s.visSamples.data();
        CHECK(s.visSamples.size() == 96);
        CHECK(vs[0] == 255 && vs[1] == 255 && vs[2] == 255 && vs[3] == 40);
        CHECK(near(f32(vs + 4), 6.14098e-05f) && near(f32(vs + 12), 4.87012f) && near(f32(vs + 16), 4.87012f));
        CHECK(vs[48] == 185 && vs[51] == 0 && near(f32(vs + 52), 8.72665e-05f) && near(f32(vs + 56), 7.43381e-05f) &&
              near(f32(vs + 60), 25));
        const efx::ElemOut& l = c.elems[1];
        const uint8_t* ld = l.def.data();
        CHECK(l.type == 8 && static_cast<uint32_t>(i32(ld)) == 0x42u && i32(ld + 48) == 200 && near(f32(ld + 56), 5) &&
              near(f32(ld + 60), 5) && ld[280] == 15);
        const uint8_t* lv = l.visSamples.data();
        CHECK(lv[0] == 51 && lv[1] == 101 && lv[2] == 101 && lv[3] == 255);  // B G R A: rgb * alpha, alpha 255 (modColorByAlpha)
        CHECK(near(f32(lv + 12), 100) && near(f32(lv + 20), 1.5f));
        size_t samples = 0;
        for (const efx::ElemOut& e : c.elems) samples += e.velSamples.size() + e.visSamples.size();
        CHECK(c.totalSize == static_cast<int32_t>(40 + 2 * 288 + std::strlen("muzzleflashes/saw_flshview_c") + 1 + samples));
    }

    // Full compile: zone_source/fx with an embedded material, a runner effect, an fx_origin.
    fs::path dir = fs::temp_directory_path() / "iw4r_fx_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "zone_source" / "fx" / "test");
    fs::create_directories(dir / "zone_source" / "fx" / "materials");
    fs::create_directories(dir / "zone_source" / "images");
    std::string flash = kTestEfx;
    flash.replace(flash.find("gfx_smk_white_atlas"), std::strlen("gfx_smk_white_atlas"), "iw4r_test_glow");
    { std::ofstream(dir / "zone_source" / "fx" / "test" / "flash.efx") << flash; }
    {
        std::ofstream(dir / "zone_source" / "fx" / "test" / "runner.efx")
            << "iwfx 2\n{\n\tname \"r\";\n\teditorFlags looping;\n\tspawnLooping 500 0;\n\tlifeSpanMsec 100 0;\n\trunner\n\t{\n\t\t\"test/flash\"\n\t};\n}\n";
    }
    { std::ofstream(dir / "zone_source" / "fx" / "materials" / "iw4r_test_glow.txt") << "blend add\nfeather 10\natlas 2 2\n"; }
    { auto png = tinyPng(); std::ofstream(dir / "zone_source" / "images" / "iw4r_test_glow.png", std::ios::binary).write(reinterpret_cast<const char*>(png.data()), png.size()); }
    Document d;
    d.addBrush(makeBox({-128, -128, -16}, {128, 128, 0}, "wc/me_concrete"));
    Sel e = d.addPointEntity("fx_origin", {0, 0, 16});
    d.ents[e.ent].set("fx", "test/runner");
    CompileOptions o = kNoBake;
    o.zoneSourceDir = (dir / "zone_source").string();
    warnings.clear();
    o.warnings = &warnings;
    std::vector<uint8_t> ff;
    CHECK(compileMapToFastfile(d, "fxtest", ff, err, MapTarget::SP, o));
    Fastfile zf;
    CHECK(readFastfile(ff, zf, err));
    zone::SimulatedZone sim;
    CHECK(zone::simulateLoad(zf.zone, sim, err));
    if (!err.empty()) std::printf("  simulateLoad (fx): %s\n", err.c_str());
    std::vector<std::string> fxNames, matNames, raw;
    for (const auto& a : sim.assets) {
        const char* n;
        std::memcpy(&n, a.header, 8);
        if (a.type == IW4_FX && !a.reference) fxNames.push_back(n);
        if (a.type == IW4_MATERIAL && !a.reference) matNames.push_back(n);
        if (a.type == IW4_RAWFILE) raw.push_back(n);
    }
    CHECK(fxNames.size() == 2 && fxNames[0] == "test/flash" && fxNames[1] == "test/runner");  // dependency first
    CHECK(std::find(matNames.begin(), matNames.end(), "iw4r_test_glow") != matNames.end());
    CHECK(std::find(raw.begin(), raw.end(), "maps/fxtest_fx.gsc") != raw.end());
    for (const auto& a : sim.assets)
        if (a.type == IW4_FX && !a.reference) {
            const uint8_t* h = static_cast<const uint8_t*>(a.header);
            const uint8_t* elems;
            std::memcpy(&elems, h + 32, 8);
            CHECK(elems != nullptr);
            const char* n;
            std::memcpy(&n, h, 8);
            if (elems && std::string(n) == "test/runner") {
                const char* runnerOf;
                std::memcpy(&runnerOf, elems + 200, 8);
                CHECK(elems[176] == 12 && runnerOf && std::string(runnerOf) == "test/flash");
            }
            if (elems && std::string(n) == "test/flash") {
                const uint8_t* mat;
                std::memcpy(&mat, elems + 200, 8);
                const char* mn = nullptr;
                if (mat) std::memcpy(&mn, mat, 8);
                CHECK(mn && std::string(mn) == ",iw4r_test_glow" && elems[172] == 1 && elems[173] == 1 && elems[174] == 4);
            }
        }
    std::printf("effects: saw_flshview_c matches stock, %zu effects + %zu materials compiled, fx script generated\n", fxNames.size(),
                matNames.size());
    fs::remove_all(dir);
}

// Shaped brushes: every shape gives valid convex brushes inside its box.
static void testShapes() {
    const Vec3 mins{-64, -32, 0}, maxs{64, 32, 96};
    shapes::Params p;
    p.sides = 12;
    p.steps = 6;
    p.thickness = 8;
    const std::map<shapes::Shape, size_t> expect = {{shapes::Shape::Cylinder, 1}, {shapes::Shape::Cone, 1}, {shapes::Shape::Pyramid, 1},
                                                    {shapes::Shape::Wedge, 1},    {shapes::Shape::Sphere, 1}, {shapes::Shape::Stairs, 6},
                                                    {shapes::Shape::Arch, 12},    {shapes::Shape::Pipe, 12}, {shapes::Shape::Room, 6}};
    for (shapes::Shape s : shapes::kAll) {
        std::vector<Brush> bs = shapes::make(s, mins, maxs, p, "caulk");
        bool ok = bs.size() == expect.at(s);
        for (Brush& b : bs) {
            ok &= b.rebuild();
            for (int k = 0; k < 3; k++) ok &= b.bounds.mins[k] >= mins[k] - 1.5 && b.bounds.maxs[k] <= maxs[k] + 1.5;
        }
        if (!ok) std::printf("  shape %s: %zu brushes\n", shapes::name(s), bs.size());
        CHECK(ok);
    }
    CHECK(shapes::make(shapes::Shape::Room, mins, maxs, {8, 8, 40}, "caulk").empty());  // walls thicker than the box
    // IW4R_SHAPES_MAP=<path>: also writes every shape side by side into a .map (for a screenshot)
    if (const char* out = std::getenv("IW4R_SHAPES_MAP")) {
        Document d;
        double x = 0;
        for (shapes::Shape s : shapes::kAll) {
            for (Brush& b : shapes::make(s, {x, 0, 0}, {x + 128, 128, 128}, p, "wc/me_concrete")) d.addBrush(std::move(b));
            x += 192;
        }
        std::string err;
        CHECK(saveMap(out, d, err));
    }
}

// Terrain: a height field of brush cells; sculpt tools; mountains; compiles like any world brushes.
static void testTerrain() {
    Document d;
    terrain::Field f;
    f.nx = 8, f.ny = 6, f.cell = 64, f.depth = 64;
    f.origin = {-256, -192, 0};
    f.material = "wc/me_concrete";
    const int te = terrain::create(d, f);
    CHECK(d.ents[te].classname() == terrain::kClass && d.ents[0].brushes.size() == 8 * 6 * 2);
    terrain::Field r;
    CHECK(terrain::read(d.ents[te], r) && r.nx == 8 && r.ny == 6 && r.h.size() == 9 * 7 && !r.id.empty());
    // raise in the middle: the centre corner goes up most, a far corner doesn't move
    terrain::Stroke s;
    s.tool = terrain::Tool::Raise;
    s.radius = 160;
    s.strength = 100;
    int i0, j0, i1, j1;
    CHECK(terrain::apply(r, s, 0, 0, 1.0, i0, j0, i1, j1));
    CHECK(r.at(4, 3) > 99 && r.at(0, 0) == 0 && r.at(4, 3) > r.at(5, 3));
    terrain::rebuildCells(d, r, i0, j0, i1, j1);
    CHECK(d.ents[0].brushes.size() == 8 * 6 * 2);  // rebuilt in place
    double top = -1e9;
    for (const Brush& b : d.ents[0].brushes) top = std::max(top, b.bounds.maxs.z);
    CHECK(std::fabs(top - r.at(4, 3)) < 0.01);
    // smooth lowers the peak, melt moves material off a steep spike, flatten levels
    const double peak = r.at(4, 3);
    s.tool = terrain::Tool::Smooth;
    terrain::apply(r, s, 0, 0, 1.0, i0, j0, i1, j1);
    CHECK(r.at(4, 3) < peak);
    terrain::Field spike = r;
    std::fill(spike.h.begin(), spike.h.end(), 0.0);
    spike.at(4, 3) = 400;
    s.tool = terrain::Tool::Melt;
    double before = 0, after = 0;
    for (double h : spike.h) before += h;
    terrain::apply(spike, s, 0, 0, 1.0, i0, j0, i1, j1);
    for (double h : spike.h) after += h;
    CHECK(spike.at(4, 3) < 400 && std::fabs(before - after) < 1e-6);  // material is moved, not lost
    s.tool = terrain::Tool::Flatten;
    s.flattenTo = 10;
    s.strength = 1000;
    for (int k = 0; k < 20; k++) terrain::apply(r, s, 0, 0, 1.0, i0, j0, i1, j1);
    CHECK(std::fabs(r.at(4, 3) - 10) < 1);
    // mountains: every brush valid, heights in range
    terrain::mountains(r, 512, 5, 7);
    double hi = 0;
    for (double h : r.h) hi = std::max(hi, h);
    CHECK(hi > 500 && hi <= 512.001);
    terrain::write(r, d.ents[te]);
    terrain::rebuildCells(d, r, 0, 0, r.nx, r.ny);
    bool valid = true;
    for (Brush& b : d.ents[0].brushes) valid &= b.rebuild();
    CHECK(valid && d.ents[0].brushes.size() == 8 * 6 * 2);
    // saved and loaded, then compiled: the terrain entity isn't in the map's entities
    std::string err;
    Document back;
    CHECK(parseMapText(writeMapText(d), back, err) && back.ents[0].brushes.size() == d.ents[0].brushes.size());
    terrain::Field rb;
    CHECK(terrain::read(back.ents[te], rb) && std::fabs(rb.at(4, 3) - r.at(4, 3)) < 0.01);
    std::vector<uint8_t> ff;
    CHECK(compileMapToFastfile(d, "terrain", ff, err, MapTarget::SP, kNoBake));
    Fastfile zf;
    CHECK(readFastfile(ff, zf, err));
    zone::SimulatedZone sim;
    CHECK(zone::simulateLoad(zf.zone, sim, err));
    auto* me = static_cast<const iw4::clipMap_t*>(sim.find(IW4_CLIPMAP_SP));
    CHECK(me && me->mapEnts && std::string(me->mapEnts->entityString).find("iw4r_terrain") == std::string::npos);
    // Paint by slope: flat -> grass, steep -> rock; rules round-trip; hand textures survive a
    // repaint and sculpting, and "force" replaces them.
    {
        terrain::Field p = r;
        p.paint = {{"ch_grass_01", 0, 20}, {"ch_cliff01", 20, 90}};
        p.paintJitter = 0;
        terrain::write(p, d.ents[te]);
        terrain::Field pr;
        CHECK(terrain::read(d.ents[te], pr) && pr.paint.size() == 2 && pr.paint[1].material == "ch_cliff01" && pr.paint[0].maxSlope == 20);
        CHECK(terrain::repaint(d, p, &r, false) > 0);
        int grass = 0, rock = 0, wrong = 0, walls = 0;
        Brush* hand = nullptr;
        for (Brush& b : d.ents[0].brushes) {
            const double slope = std::acos(std::fabs(b.faces[0].normal.z)) * 180 / 3.14159265358979;
            const std::string& m = b.faces[0].material;
            (m == "ch_grass_01" ? grass : rock)++;
            wrong += (slope < 19.9 && m != "ch_grass_01") || (slope > 20.1 && m != "ch_cliff01");
            for (size_t k = 1; k < b.faces.size(); k++)
                if (b.faces[k].material != "caulk") walls++, wrong += b.faces[k].material != "ch_cliff01";
            if (!hand) hand = &b;
        }
        CHECK(grass > 0 && rock > 0 && walls > 0 && wrong == 0);
        hand->faces[0].material = "hand_set";
        std::string hid;
        int hi_, hj, ht;
        terrain::parseTag(*hand, hid, hi_, hj, ht);
        terrain::repaint(d, p, nullptr, false);
        CHECK(hand->faces[0].material == "hand_set");
        terrain::rebuildCells(d, p, 0, 0, p.nx, p.ny);  // as sculpting does
        bool kept = false;
        for (const Brush& b : d.ents[0].brushes) {
            std::string id;
            int i, j, t;
            if (terrain::parseTag(b, id, i, j, t) && i == hi_ && j == hj && t == ht) kept = b.faces[0].material == "hand_set";
        }
        CHECK(kept);
        // renaming a rule's material repaints its cells (old name is "automatic" via prev)
        terrain::Field p2 = p;
        p2.paint[1].material = "ch_cliff02";
        terrain::repaint(d, p2, &p, false);
        int oldRock = 0, newRock = 0;
        for (const Brush& b : d.ents[0].brushes) oldRock += b.faces[0].material == "ch_cliff01", newRock += b.faces[0].material == "ch_cliff02";
        CHECK(oldRock == 0 && newRock >= rock - 1 && newRock > 0);  // less the hand-set one
        terrain::repaint(d, p2, nullptr, true);
        int handLeft = 0;
        for (const Brush& b : d.ents[0].brushes) handLeft += b.faces[0].material == "hand_set";
        CHECK(handLeft == 0);
        // jitter only moves borders a little; a height rule paints peaks
        p2.paintJitter = 6;
        p2.paint.insert(p2.paint.begin(), terrain::PaintRule{"peak", 0, 90, 400, 1e6});
        terrain::repaint(d, p2, nullptr, true);
        int peaks = 0;
        bool peaksHigh = true;
        for (const Brush& b : d.ents[0].brushes)
            if (b.faces[0].material == "peak") {
                peaks++;
                double z = 0;
                for (const Vec3& q : b.faces[0].winding) z += q.z;
                peaksHigh &= z / b.faces[0].winding.size() - p2.origin.z >= 400 - 1e-6;
            }
        CHECK(peaks > 0 && peaksHigh);
        terrain::Field none = p2;  // clearing the rules removes the keys
        none.paint.clear();
        terrain::write(none, d.ents[te]);
        CHECK(!d.ents[te].find("paint"));
        terrain::write(r, d.ents[te]);
        terrain::repaint(d, r, &p2, true);
    }
    // Paint brush: paints inside the radius only, marks the brushes, survives slope repaint and
    // sculpting (with the surface attribute too), honours the slope filter, Ctrl-erase restores.
    {
        terrain::Field p = r;
        p.paint = {{"ch_grass_01", 0, 90}};
        terrain::write(p, d.ents[te]);
        terrain::repaint(d, p, nullptr, true);
        terrain::Stroke pb_;
        pb_.tool = terrain::Tool::Paint;
        pb_.material = "brushed";
        pb_.radius = 100;
        pb_.scatter = 0;
        const int n = terrain::paint(d, p, pb_, 0, 0, false);
        int inside = 0, outsidePainted = 0;
        for (const Brush& b : d.ents[0].brushes) {
            Vec3 c;
            for (const Vec3& q : b.faces[0].winding) c += q;
            c = c * (1.0 / b.faces[0].winding.size());
            const bool in = std::hypot(c.x, c.y) <= 100;
            inside += in;
            if (!in) outsidePainted += b.faces[0].material == "brushed";
            if (in) CHECK(b.faces[0].material == "brushed" && terrain::painted(b));
        }
        CHECK(n == inside && n > 0 && outsidePainted == 0);
        CHECK(terrain::paint(d, p, pb_, 0, 0, false) == 0);  // painting again changes nothing
        for (Brush& b : d.ents[0].brushes)
            if (terrain::painted(b)) b.setSurfaceType("grass");
        terrain::repaint(d, p, nullptr, false);
        terrain::Stroke up;
        up.tool = terrain::Tool::Raise;
        up.radius = 300;
        up.strength = 200;
        terrain::apply(p, up, 0, 0, 1.0, i0, j0, i1, j1);
        terrain::rebuildCells(d, p, i0, j0, i1, j1);
        int still = 0, surf = 0;
        for (const Brush& b : d.ents[0].brushes)
            if (b.faces[0].material == "brushed") still += terrain::painted(b), surf += b.surfaceType() == "grass";
        CHECK(still == n && surf == n);
        // slope filter: only steep triangles take the paint
        pb_.material = "steep";
        pb_.minSlope = 30;
        pb_.radius = 10000;
        terrain::paint(d, p, pb_, 0, 0, false);
        bool filtered = true;
        int steep = 0;
        for (const Brush& b : d.ents[0].brushes) {
            const double slope = std::acos(std::fabs(b.faces[0].normal.z)) * 180 / 3.14159265358979;
            if (b.faces[0].material == "steep") steep++, filtered &= slope >= 30 - 1e-6;
            else filtered &= slope < 30 + 1e-6 || b.faces[0].material == "steep";
        }
        CHECK(filtered && steep > 0);
        // Ctrl erase: everything back to the rule, no marks
        terrain::paint(d, p, pb_, 0, 0, true);
        int left = 0;
        for (const Brush& b : d.ents[0].brushes) left += terrain::painted(b) || b.faces[0].material != "ch_grass_01";
        CHECK(left == 0);
        // the mark round-trips through the .map
        pb_.material = "brushed";
        pb_.minSlope = 0;
        pb_.radius = 100;
        terrain::paint(d, p, pb_, 0, 0, false);
        std::string perr;
        Document pb;
        CHECK(parseMapText(writeMapText(d), pb, perr));
        int marks = 0;
        for (const Brush& b : pb.ents[0].brushes) marks += terrain::painted(b);
        CHECK(marks == n);
        r = p;
        r.paint.clear();
        terrain::write(r, d.ents[te]);
    }
    if (const char* out = std::getenv("IW4R_TERRAIN_MAP")) {  // a bigger one, for a screenshot
        Document big;
        terrain::Field bf;
        bf.nx = bf.ny = 32;
        bf.cell = 64;
        bf.origin = {-1024, -1024, 0};
        bf.material = "ch_ground_dirt03";
        bf.paint = terrain::paintPresets()[0].rules;
        bf.paintJitter = 4;
        const int be = terrain::create(big, bf);
        terrain::read(big.ents[be], bf);
        terrain::mountains(bf, 768, 5, 3);
        terrain::write(bf, big.ents[be]);
        terrain::rebuildCells(big, bf, 0, 0, bf.nx, bf.ny);
        CHECK(saveMap(out, big, err));
    }
}

// Effect preview: a hand-written effect in a temp zone_source runs from an fx_origin: looping
// spawn rate, lifetimes, spawn volume in the fx_origin's axes, graphs, atlas frames, one-shot replay.
static void testFxPreview() {
    namespace fs = std::filesystem;
    const fs::path zs = fs::temp_directory_path() / "iw4r_fxpreview" / "zone_source";
    std::error_code ec;
    fs::remove_all(zs.parent_path(), ec);
    fs::create_directories(zs / "fx" / "test", ec);
    auto write = [&](const char* name, const std::string& text) { std::ofstream(zs / "fx" / "test" / name) << text; };
    // looping: one sprite per 100 ms, 1 s life, spawned 50 up (X = up for the default angles),
    // y/z in -100..100, rising 20 u/s, size 10 -> 30, colour red -> blue, 2x2 atlas over life
    write("loop.efx", R"(iwfx 3
{
	name "a";
	editorFlags looping;
	flags spawnRelative;
	spawnLooping 100 0;
	lifeSpanMsec 1000 0;
	spawnOrgX 50 0;
	spawnOrgY -100 200;
	spawnOrgZ -100 200;
	atlasBehavior playOverLife;
	velGraph0X 20 { { 0 1 1 1 } { 0 1 1 1 } };
	sizeGraph0 10 { { 0 1 1 3 } { 0 1 1 3 } };
	colorGraph 1 { { 0 1 0 0 1 0 0 1 } { 0 1 0 0 1 0 0 1 } };
	alphaGraph 1 { { 0 1 1 1 } { 0 1 1 1 } };
	billboardSprite { "mat_loop" };
}
)");
    // one-shot: 5 sprites, 0.5 s life, replayed every `delay` seconds
    write("burst.efx", R"(iwfx 3
{
	name "b";
	editorFlags;
	spawnOneShot 5 0;
	lifeSpanMsec 500 0;
	sizeGraph0 4 { { 0 1 1 1 } { 0 1 1 1 } };
	billboardSprite { "mat_burst" };
}
{
	name "skipped";
	editorFlags;
	spawnOneShot 5 0;
	billboardSprite { "missing_material" };
}
)");
    fxpreview::Preview pv;
    std::vector<std::string> warnings;
    pv.load(zs.string(),
            [](const std::string& m) {
                fxpreview::MaterialInfo mi;
                mi.found = m != "missing_material";
                mi.additive = m == "mat_loop";
                mi.rows = mi.cols = m == "mat_loop" ? 2 : 1;
                return mi;
            },
            warnings);
    CHECK(pv.effectCount() == 2 && warnings.empty());
    Document d;
    Sel a = d.addPointEntity("fx_origin", {1000, 2000, 300});
    d.ents[a.ent].set("fx", "test/loop");
    Sel b = d.addPointEntity("fx_origin", {0, 0, 0});
    d.ents[b.ent].set("fx", "test/burst");
    d.ents[b.ent].set("delay", "1");
    Sel c = d.addPointEntity("fx_origin", {0, 0, 0});
    d.ents[c.ent].set("fx", "test/nope");
    pv.sync(d);
    CHECK(pv.emitterCount() == 2 && pv.missing().count("test/nope") == 1);
    pv.warmup(3.0);
    // steady state: ~10 looping (1 s / 100 ms) + 5 burst (still alive 0..0.5 s after a replay, or not)
    const size_t n = pv.particleCount();
    if (n < 9 || n > 16) std::printf("  fx preview particles %zu\n", n);
    CHECK(n >= 9 && n <= 16);
    std::vector<fxpreview::Sprite> sp;
    const Vec3 eye{-5000, 0, 0};  // looking along +x at everything
    pv.sprites(eye, {1, 0, 0}, {0, -1, 0}, {0, 0, 1}, sp);
    int loops = 0, bursts = 0;
    bool inVolume = true, sized = true, colours = true, atlas = true, modes = true;
    for (const fxpreview::Sprite& s : sp) {
        if (s.material == "mat_loop") {
            loops++;
            // spawned at z 350, rising 20 u/s for up to 1 s; the effect's Y/Z (left/up, for the default
            // angles world y and -x) within +-100
            inVolume &= s.pos.z >= 349 && s.pos.z <= 371 && std::fabs(s.pos.y - 2000) <= 100.01 && std::fabs(s.pos.x - 1000) <= 100.01;
            const double w = length(s.right) * 2;
            const double age = (s.pos.z - 350) / 20;  // 0..1
            sized &= std::fabs(w - (10 + 20 * age)) < 1.5;
            colours &= std::fabs(s.color[0] - (1 - age)) < 0.08 && std::fabs(s.color[2] - age) < 0.08;
            const int frame = static_cast<int>(s.uv[0] * 2 + 0.01) + 2 * static_cast<int>(s.uv[1] * 2 + 0.01);
            atlas &= frame == std::min(3, static_cast<int>(age * 4 + 1e-3)) || std::fabs(age * 4 - std::round(age * 4)) < 0.15;
            modes &= s.additive;
        } else {
            bursts++;
            modes &= !s.additive && s.material == "mat_burst";
        }
    }
    if (!(inVolume && sized && colours && atlas && modes))
        std::printf("  fx preview: volume %d size %d colour %d atlas %d modes %d\n", inVolume, sized, colours, atlas, modes);
    CHECK(loops >= 9 && inVolume && sized && colours && atlas && modes);
    // the one-shot replays every second: after the burst dies (0.5 s) and comes back
    int seen = 0, gone = 0;
    for (int k = 0; k < 60; k++) {
        pv.update(1.0 / 30);
        std::vector<fxpreview::Sprite> s2;
        pv.sprites(eye, {1, 0, 0}, {0, -1, 0}, {0, 0, 1}, s2);
        int nb = 0;
        for (const fxpreview::Sprite& s : s2) nb += s.material == "mat_burst";
        seen += nb == 5;
        gone += nb == 0;
    }
    CHECK(seen > 5 && gone > 5);
    // moving the fx_origin restarts only that emitter; removing one drops it
    d.ents[a.ent].setOrigin({0, 0, 0});
    pv.sync(d);
    CHECK(pv.emitterCount() == 2);
    d.ents.erase(d.ents.begin() + b.ent);
    pv.sync(d);
    CHECK(pv.emitterCount() == 1);
    // the shipped firefly example parses and runs
    fxpreview::Preview ff;
    ff.load("maps/zm_test/zone_source", [](const std::string&) { return fxpreview::MaterialInfo{}; }, warnings);
    Document fd;
    Sel f = fd.addPointEntity("fx_origin", {0, 0, 0});
    fd.ents[f.ent].set("fx", "env/nature/fx_fireflies");
    ff.sync(fd);
    ff.warmup(10);
    std::vector<fxpreview::Sprite> fly;
    ff.sprites({-2000, 0, 50}, {1, 0, 0}, {0, -1, 0}, {0, 0, 1}, fly);
    bool flies = ff.particleCount() > 30;
    for (const fxpreview::Sprite& s : fly) flies &= s.pos.z > 0 && s.pos.z < 200 && std::fabs(s.pos.x) < 400 && std::fabs(s.pos.y) < 400;
    CHECK(flies && !fly.empty());

    // Dynamic effects: a trigger_radius aimed at a named fx_origin starts it when the viewer walks in.
    {
        fxpreview::Preview pd;
        pd.load(zs.string(), [](const std::string& m) { return fxpreview::MaterialInfo{m != "missing_material", true, 1, 1}; }, warnings);
        Document dd;
        Sel lf = dd.addPointEntity("fx_origin", {0, 0, 0});  // looping, triggered, 1.5 s each time
        dd.ents[lf.ent].set("fx", "test/loop");
        dd.ents[lf.ent].set("targetname", "fire");
        dd.ents[lf.ent].set("duration", "1.5");
        Sel bf = dd.addPointEntity("fx_origin", {500, 0, 0});  // one-shot, script-started only, once
        dd.ents[bf.ent].set("fx", "test/burst");
        dd.ents[bf.ent].set("targetname", "boom");
        dd.ents[bf.ent].set("start_off", "1");
        dd.ents[bf.ent].set("once", "1");
        Sel st = dd.addPointEntity("fx_origin", {-500, 0, 0});  // static: unaffected
        dd.ents[st.ent].set("fx", "test/loop");
        Sel tr = dd.addPointEntity("trigger_radius", {0, 1000, 0});
        dd.ents[tr.ent].set("target", "fire");
        dd.ents[tr.ent].set("radius", "100");
        dd.ents[tr.ent].set("height", "50");
        pd.sync(dd);
        pd.setViewer({0, 0, 500});
        pd.warmup(2.0);
        auto count = [&](const char* mat, double x) {
            std::vector<fxpreview::Sprite> s;
            pd.sprites({-5000, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 0, 1}, s);
            int n = 0;
            for (const fxpreview::Sprite& q : s) n += q.material == mat && std::fabs(q.pos.x - x) < 200;
            return n;
        };
        const bool idle = count("mat_loop", 0) == 0 && count("mat_burst", 500) == 0 && count("mat_loop", -500) > 5;
        pd.setViewer({0, 1050, 20});  // walk in
        pd.warmup(1.0);
        const bool started = count("mat_loop", 0) > 5;
        pd.warmup(2.0);  // past the 1.5 s duration + 1 s particle life, still standing inside: no restart
        const bool stopped = count("mat_loop", 0) == 0;
        pd.setViewer({0, 0, 500});
        pd.setViewer({0, 1000, 10});  // walk out and back in: runs again
        pd.warmup(0.5);
        const bool again = count("mat_loop", 0) > 2;
        pd.startGroup("boom");  // level notify("boom")
        pd.update(1.0 / 30);
        const bool boom = count("mat_burst", 500) == 5;
        pd.warmup(1.0);
        pd.startGroup("boom");  // once: no second burst
        pd.update(1.0 / 30);
        const bool onceOk = count("mat_burst", 500) == 0;
        if (!(idle && started && stopped && again && boom && onceOk))
            std::printf("  fx dynamic: idle %d started %d stopped %d again %d boom %d once %d\n", idle, started, stopped, again, boom, onceOk);
        CHECK(idle && started && stopped && again && boom && onceOk);
        CHECK(pd.groups() == std::set<std::string>({"boom", "fire"}));

        // compiler: placements + the generated script
        std::vector<std::string> w;
        Sel bt = dd.addPointEntity("trigger_radius", {0, 0, 0});  // no radius/height: defaults
        dd.ents[bt.ent].set("target", "boom");
        dd.ents[bt.ent].set("script_noteworthy", "stop");
        std::vector<FxPlacement> pl = fxPlacements(dd, {{"test/loop", true}, {"test/burst", false}}, &w);
        CHECK(pl.size() == 3);
        CHECK(pl[0].name == "fire" && pl[0].waitStart && pl[0].duration == 1.5f && pl[0].looping && pl[0].triggers.size() == 1);
        CHECK(pl[1].name == "boom" && pl[1].waitStart && pl[1].once && !pl[1].looping && pl[1].delay == 0);
        CHECK(pl[2].name.empty() && !pl[2].waitStart && pl[2].delay == 0);
        CHECK(dd.ents[bt.ent].find("radius") && *dd.ents[bt.ent].find("radius") == "64" && w.size() == 2);
        const std::string gsc = fxPlacementScript(pl);
        auto has = [&](const char* t) { return gsc.find(t) != std::string::npos; };
        CHECK(has("thread dynamic(0, (0, 0, 0), (0, 0, 1), (-1, 0, 0), 0, \"fire\", 1.5, 0, 1);"));
        CHECK(has("thread dynamic(1, (500, 0, 0)") && has("\"boom\", 0, 1, 1);"));
        CHECK(has("thread play(0, (-500, 0, 0)"));
        CHECK(has("thread watch(\"fire\");") && has("thread watch(\"boom\");"));
        CHECK(has("level._iw4r_fxloop[0] = true;") && has("level._iw4r_fxloop[1] = false;"));
        CHECK(gsc.find("level._iw4r_fxloop = [];") < gsc.find("thread place();"));
        // braces balance (a cheap syntax sanity check of the generated text)
        int depth = 0, paren = 0;
        bool neg = false;
        for (char ch : gsc) {
            depth += ch == '{' ? 1 : ch == '}' ? -1 : 0;
            paren += ch == '(' ? 1 : ch == ')' ? -1 : 0;
            neg |= depth < 0 || paren < 0;
        }
        CHECK(depth == 0 && paren == 0 && !neg);
        // static-only maps keep the old short script
        std::vector<FxPlacement> only = {pl[2]};
        CHECK(fxPlacementScript(only).find("dynamic(") == std::string::npos);
    }
    fs::remove_all(zs.parent_path(), ec);
}

// Pen tool: outlines -> convex pieces (area preserved, each convex) -> valid brushes.
static void testPenOutlines() {
    using P = shapes::Point2;
    auto area = [](const std::vector<P>& p) {
        double a = 0;
        for (size_t i = 0; i < p.size(); i++) a += p[i][0] * p[(i + 1) % p.size()][1] - p[(i + 1) % p.size()][0] * p[i][1];
        return a * 0.5;
    };
    auto check = [&](const char* name, const std::vector<P>& outline, size_t maxPieces) {
        std::string err;
        auto pieces = shapes::convexPieces(outline, err);
        double sum = 0;
        bool convex = true;
        for (const auto& pc : pieces) {
            sum += area(pc);
            for (size_t i = 0; i < pc.size(); i++) {
                const P &a = pc[(i + pc.size() - 1) % pc.size()], &b = pc[i], &c = pc[(i + 1) % pc.size()];
                convex &= (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]) > 0;
            }
        }
        const bool ok = !pieces.empty() && pieces.size() <= maxPieces && convex && std::fabs(sum - std::fabs(area(outline))) < 1e-6;
        if (!ok) std::printf("  pen %s: %zu pieces, area %g vs %g, convex %d, %s\n", name, pieces.size(), sum, area(outline), convex, err.c_str());
        CHECK(ok);
        return pieces.size();
    };
    CHECK(check("square", {{0, 0}, {128, 0}, {128, 128}, {0, 128}}, 1) == 1);
    CHECK(check("L", {{0, 0}, {128, 0}, {128, 32}, {32, 32}, {32, 128}, {0, 128}}, 2) == 2);
    check("U", {{0, 0}, {192, 0}, {192, 128}, {128, 128}, {128, 32}, {64, 32}, {64, 128}, {0, 128}}, 4);
    std::vector<P> star;
    for (int i = 0; i < 10; i++) {
        const double a = 3.14159265358979 * i / 5, r = i % 2 ? 40 : 100;
        star.push_back({std::cos(a) * r, std::sin(a) * r});
    }
    check("star", star, 8);
    // IW4R_PEN_MAP=<path>: the star extruded as the pen would (Top view, 0..64), for a screenshot
    if (const char* out = std::getenv("IW4R_PEN_MAP")) {
        std::vector<P> big;
        for (const P& q : star) big.push_back({std::round(q[0] * 2.56) , std::round(q[1] * 2.56)});
        std::string perr;
        Document d;
        for (Brush& b : shapes::fromOutline(big, 0, 1, 2, 0, 64, "wc/me_concrete", perr)) d.addBrush(std::move(b));
        CHECK(d.ents[0].brushes.size() > 1 && saveMap(out, d, perr));
    }
    check("clockwise L", {{0, 128}, {32, 128}, {32, 32}, {128, 32}, {128, 0}, {0, 0}}, 2);
    CHECK(check("collinear", {{0, 0}, {64, 0}, {128, 0}, {128, 128}, {0, 128}}, 1) == 1);
    std::string err;
    CHECK(shapes::convexPieces({{0, 0}, {128, 128}, {128, 0}, {0, 128}}, err).empty() && err.find("crosses") != std::string::npos);
    CHECK(shapes::convexPieces({{0, 0}, {64, 0}, {128, 0}}, err).empty());
    // extruded in the Top view (u x, v y, w z) and the Front view (u x, v z, w y)
    auto brushes = shapes::fromOutline({{0, 0}, {128, 0}, {128, 32}, {32, 32}, {32, 128}, {0, 128}}, 0, 1, 2, 0, 64, "caulk", err);
    bool ok = brushes.size() == 2;
    for (Brush& b : brushes) ok &= b.rebuild() && b.bounds.mins.z == 0 && b.bounds.maxs.z == 64;
    CHECK(ok);
    brushes = shapes::fromOutline({{0, 0}, {64, 0}, {64, 96}, {0, 96}}, 0, 2, 1, 16, 48, "caulk", err);
    CHECK(brushes.size() == 1 && brushes[0].bounds.mins.y == 16 && brushes[0].bounds.maxs.y == 48 && brushes[0].bounds.maxs.z == 96);
}

// Skeleton conversion (BO1 / CoD4 -> IW4): names, tags, the 128-bone limit with weights merged.
static void testSkeletonConvert() {
    xmodelexport::Model m;
    auto bone = [&](const char* n, int parent, float x, float y, float z) {
        xmodelexport::Bone b;
        b.name = n;
        b.parent = parent;
        b.offset[0] = x, b.offset[1] = y, b.offset[2] = z;
        m.bones.push_back(b);
        return static_cast<int>(m.bones.size()) - 1;
    };
    const int root = bone("TAG_ORIGIN", -1, 0, 0, 0);
    const int mainroot = bone("J_MainRoot", root, 0, 0, 40);
    const int spine = bone("J_Spine4", mainroot, 0, 0, 55);
    const int head = bone("J_Head", spine, 0, 0, 64);
    bone("J_Wrist_RI", spine, 10, -20, 40);
    bone("J_EyeBall_LE", head, 3, 1, 66);
    bone("J_EyeBall_RI", head, 3, -1, 66);
    bone("J_Brow_Out_LE", head, 3, 2, 68);
    // A BO1-style face rig MW2 never animates, every bone skinned (so some weighted bones must go).
    const int firstExtra = static_cast<int>(m.bones.size());
    for (int i = 0; i < 132; i++) {
        bone(("J_Face_Extra_" + std::to_string(i)).c_str(), head, 3, 0, 65);
        xmodelexport::Vertex v;
        v.weights = {{firstExtra + i, 1.0f}};
        m.verts.push_back(v);
    }
    xmodelexport::Face f;
    f.v[0].vert = 0, f.v[1].vert = 1, f.v[2].vert = 2;
    m.faces.push_back(f);
    m.objects.push_back("o");
    m.materials.push_back({"mtl", "Lambert", "c:/x.tga"});
    skeleton::Report r;
    std::string err;
    CHECK(skeleton::convertForIw4(m, r, err));
    CHECK(r.kind == "character" && r.bonesBefore == 140 && m.bones.size() <= skeleton::kMaxBones);
    auto has = [&](const char* n) {
        for (const auto& b : m.bones)
            if (b.name == n) return true;
        return false;
    };
    CHECK(has("tag_origin") && has("j_head") && has("tag_eye") && has("tag_weapon_right") && has("j_browouter_le") && !has("J_Head"));
    // the removed (weighted) face bone's weight went to j_head: one bone, weight 1
    int headNow = -1;
    for (size_t i = 0; i < m.bones.size(); i++)
        if (m.bones[i].name == "j_head") headNow = static_cast<int>(i);
    CHECK(m.verts[0].weights.size() == 1 && m.verts[0].weights[0].first == headNow && std::fabs(m.verts[0].weights[0].second - 1) < 1e-5f);
    for (const auto& b : m.bones) CHECK(b.parent < static_cast<int>(m.bones.size()));
    // written and read back
    xmodelexport::Model back;
    CHECK(xmodelexport::parse(xmodelexport::format(m, {"Export filename: 'C:/projects/cod/t5/model_export/x.XMODEL_EXPORT'"}), back, err));
    CHECK(back.bones.size() == m.bones.size() && back.verts.size() == 132 && back.faces.size() == 1 && back.materials[0].texture == "c:/x.tga");
    // converting again changes nothing
    skeleton::Report again;
    CHECK(skeleton::convertForIw4(back, again, err) && !again.changed());
    CHECK(skeleton::detectSource("// Export filename: 'C:/projects/cod/t5/model_export/x'") == skeleton::Source::Bo1);
    // The real thing, when Black Ops is installed: base_character (147 bones).
    const char* bo1 = "C:/Program Files (x86)/Steam/steamapps/common/Call of Duty Black Ops/model_export/base_character/base_character.XMODEL_EXPORT";
    xmodelexport::Model bc;
    if (std::filesystem::exists(bo1) && xmodelexport::load(bo1, bc, err)) {
        skeleton::Report br;
        CHECK(skeleton::convertForIw4(bc, br, err) && bc.bones.size() <= skeleton::kMaxBones);
        std::printf("skeleton: BO1 base_character %s\n", br.summary().c_str());
        modelbuild::Model built;
        CHECK(modelbuild::build(bc, "base_character", built, err));
    }
}

// Brush surface type override: own clipMap material entry with surfaceFlags type << 20.
static void testSurfaceTypes() {
    CHECK(surfaceTypeIndex("metal") == 13 && surfaceTypeIndex("slush") == 30 && surfaceTypeIndex("") == -1);
    Document d;
    d.addBrush(makeBox({-128, -128, -16}, {128, 128, 0}, "wc/me_concrete"));
    d.addBrush(makeBox({200, -128, -16}, {400, 128, 0}, "wc/me_concrete"));
    d.ents[0].brushes[1].setSurfaceType("metal");
    CHECK(d.ents[0].brushes[1].surfaceType() == "metal" && d.ents[0].brushes[0].surfaceType().empty());
    d.ents[0].brushes[1].setSurfaceType("wood");  // replaces, doesn't stack
    CHECK(d.ents[0].brushes[1].surfaceType() == "wood" && d.ents[0].brushes[1].attrs.size() == 1);
    std::vector<uint8_t> ff;
    std::string err;
    CHECK(compileMapToFastfile(d, "surf", ff, err, MapTarget::SP, kNoBake));
    Fastfile zf;
    CHECK(readFastfile(ff, zf, err));
    zone::SimulatedZone sim;
    CHECK(zone::simulateLoad(zf.zone, sim, err));
    auto* cm = static_cast<const iw4::clipMap_t*>(sim.find(IW4_CLIPMAP_SP));
    int wood = 0, other = 0;
    if (cm)
        for (unsigned i = 0; i < cm->numMaterials; i++) {
            if (std::string(cm->materials[i].name) != "me_concrete") continue;
            ((cm->materials[i].surfaceFlags >> 20) == 21 ? wood : other)++;
        }
    CHECK(cm && wood == 1 && other == 1);
}

// A custom map script (<map>.gsc next to the .map) replaces the generated one in the zone.
// Weapon pack (SP): the zone also carries level so_w_<map>'s script; the pack's add-on lump is renamed.
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
    CHECK(!sim.find(IW4_ADDON_MAP_ENTS));  // the pack has the (single) add-on lump
    // deployWeaponPack's rename of the pack's lump: same bytes, new name, blank entities.
    std::vector<uint8_t> pack = buildZoneImage({buildAddonMapEnts("maps/so_juggernauts_favela.mapents",
                                                                  "{\n\"classname\" \"worldspawn\"\n}\n"),
                                                buildMapEnts("after", "{\n}\n")});
    const size_t packSize = pack.size();
    CHECK(retargetAddonMapEnts(pack, "maps/so_juggernauts_favela.mapents", "maps/so_w_basic.mapents", err));
    CHECK(pack.size() == packSize);
    zone::SimulatedZone psim;
    CHECK(zone::simulateLoad(pack, psim, err));
    auto* ame = static_cast<const iw4::AddonMapEnts*>(psim.find(IW4_ADDON_MAP_ENTS));
    CHECK(ame && std::string(ame->name) == "maps/so_w_basic.mapents" &&
          ame->numEntityChars == 35 + 30 - 24 && std::string(ame->entityString).find_first_not_of('\n') ==std::string::npos);
    auto* me = static_cast<const iw4::MapEnts*>(psim.find(IW4_MAPENTS));
    CHECK(me && std::string(me->name) == "after");
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
    // the misc_model became a static model (not an entity)
    auto* me = static_cast<const iw4::MapEnts*>(sim.find(IW4_MAPENTS));
    CHECK(me && std::string(me->entityString).find("misc_model") == std::string::npos);
    auto* gw = static_cast<const iw4::GfxWorld*>(sim.find(IW4_GFXWORLD));
    // (the draw inst names the model by reference; the game resolves it to the asset above)
    CHECK(gw && gw->dpvs.smodelCount == 1 && gw->dpvs.smodelDrawInsts && gw->dpvs.smodelDrawInsts[0].model &&
          std::string(gw->dpvs.smodelDrawInsts[0].model->name) == ",iw4r_test_crate");
    fs::remove_all(dir);
}

// Patches (parse, tessellate, faces, collision), brush models ("*N": cmodel + GfxBrushModel,
// origin-relative), static models (GfxWorld draw insts, clipMap static models + tree, player clip)
// and model collision surfaces, end to end through the loader simulation.
static const char* kCurvePatch =
    "{\n curve\n {\n  contents detail;\n  wc/me_concrete\n  lightmap_gray\n  3 3 16 8\n"
    "  (\n   v 0 -64 0 t 0 0 0 0\n   v 0 0 0 t 0 256 0 0\n   v 0 64 0 t 0 512 0 0\n  )\n"
    "  (\n   v 128 -64 0 t 256 0\n   v 128 0 0 c 255 0 0 128 t 256 256\n   v 128 64 0 t 256 512\n  )\n"
    "  (\n   v 128 -64 128 t 512 0 1 1\n   v 128 0 128 t 512 256 1 1\n   v 128 64 128 t 512 512 1 1\n  )\n }\n}\n";

static void testPatches() {
    patch::Patch p;
    std::string err;
    CHECK(patch::parse(kCurvePatch, p, &err));
    if (!err.empty()) std::printf("  patch parse: %s\n", err.c_str());
    CHECK(p.curve && p.rows == 3 && p.cols == 3 && p.verts.size() == 9 && p.material == "wc/me_concrete" && !p.nonColliding);
    CHECK(p.hasColor && p.at(1, 1).rgba[0] == 255 && p.at(1, 1).rgba[1] == 0 && p.at(1, 1).rgba[3] == 128);
    CHECK(p.at(2, 0).p.z == 128 && p.at(1, 2).st[1] == 512);
    // columns run along y (flat), rows bend from the floor up the wall: subdivided only that way
    patch::Grid g = patch::tessellate(p, [](const std::string&, double& w, double& h) { w = h = 256; });
    CHECK(g.w == 2 && g.h > 3 && g.h <= 17);
    // corners interpolate, uv normalized by the texture size
    CHECK(length(g.p[g.at(0, 0)] - Vec3{0, -64, 0}) < 1e-9 && length(g.p[g.at(g.h - 1, g.w - 1)] - Vec3{128, 64, 128}) < 1e-9);
    CHECK(std::fabs(g.uv[2 * g.at(g.h - 1, g.w - 1)] - 2.0) < 1e-9 && std::fabs(g.uv[2 * g.at(g.h - 1, g.w - 1) + 1] - 2.0) < 1e-9);
    // front = cross(d/dcol, d/drow): +y x +x = -z on the floor part, +y x +z = +x up the wall
    CHECK(g.n[g.at(0, 0)].z < -0.9 && g.n[g.at(g.h - 1, 0)].x > 0.9);
    std::vector<Face> fs = patch::faces(g, p.material);
    CHECK(!fs.empty());
    bool ok = true;
    for (const Face& f : fs) {
        ok &= f.uvs.size() == 2 * f.winding.size() && f.vnormals.size() == f.winding.size() && f.colors.size() == f.winding.size();
        for (const Vec3& v : f.winding) ok &= std::fabs(dot(f.normal, v) - f.dist) < 0.05;
        ok &= dot(f.normal, f.vnormals[0]) > 0.5;  // smooth normals on the same side as the face
    }
    CHECK(ok);
    std::vector<Brush> cb = patch::collisionBrushes(p);
    CHECK(!cb.empty() && cb.size() <= 2 * 4 * 1 + 8);
    for (const Brush& b : cb) CHECK(b.bounds.valid());
    // a mesh keeps its grid; a non-colliding one has no brushes
    const std::string mesh = "{\n mesh\n {\n  contents nonColliding;\n  caulk\n  lightmap_gray\n  2 2 16 8\n"
                             "  (\n   v 0 0 0 t 0 0\n   v 64 0 0 t 64 0\n  )\n  (\n   v 0 64 0 t 0 64\n   v 64 64 0 t 64 64\n  )\n }\n}\n";
    patch::Patch m;
    CHECK(patch::parse(mesh, m, &err) && !m.curve && m.nonColliding && patch::collisionBrushes(m).empty());
    patch::Grid mg = patch::tessellate(m, nullptr);
    CHECK(mg.w == 2 && mg.h == 2 && patch::faces(mg, "caulk").size() == 1);  // one planar quad
    CHECK(mg.n[0].z > 0.9);                                                // x then y: +z
    patch::Patch bad;
    CHECK(!patch::parse("{\n curve\n {\n  caulk\n  lightmap_gray\n  2 2 16 8\n }\n}\n", bad, &err));

    // 26-DOP hull
    Brush hull;
    std::vector<Vec3> cube;
    for (int i = 0; i < 8; i++) cube.push_back({i & 1 ? 10.0 : -10.0, i & 2 ? 20.0 : -20.0, i & 4 ? 5.0 : 0.0});
    CHECK(worldgeom::dopBrush(cube, "clip", hull) && hull.faces.size() == 6 && hull.bounds.maxs.y == 20 && hull.bounds.mins.z == 0);

    // placement: CoD AnglesToAxis (yaw 90: forward = +y)
    Entity pe;
    pe.set("origin", "1 2 3");
    pe.set("angles", "0 90 0");
    pe.set("modelscale", "2");
    worldgeom::Placement pl = worldgeom::entityPlacement(pe);
    CHECK(std::fabs(pl.axis[0].y - 1) < 1e-9 && std::fabs(pl.axis[1].x + 1) < 1e-9 && pl.scale == 2);
    CHECK(length(pl.apply({1, 0, 0}) - Vec3{1, 4, 3}) < 1e-9);
}

// The bake covers patches and brush models, and static-model occluders cast shadows: patch faces
// get lightmaps, brush-model faces keep the sun baked in (they're drawn without a primary light),
// an occluder above the floor leaves a sun shadow under it.
static void testGeometryBake() {
    Document d;
    d.ents[0].set("sundirection", "-89 0 0");  // straight down
    d.ents[0].set("sunlight", "1.2");
    d.addBrush(makeBox({-512, -512, -16}, {512, 512, 0}, "wc/me_concrete"));
    d.ents[0].rawBlocks.push_back(kCurvePatch);
    Entity door;
    door.set("classname", "script_brushmodel");
    door.brushes.push_back(makeBox({300, -32, 0}, {340, 32, 40}, "wc/me_concrete"));
    d.ents.push_back(door);
    light::Settings s;
    s.skySamples = 8;
    s.bounces = 0;
    s.gpu = false;
    // A static model's triangles: a 200x200 roof at z 64 over (-350..-150, -350..-150)
    const Vec3 a{-350, -350, 64}, b{-150, -350, 64}, c{-150, -150, 64}, e{-350, -150, 64};
    s.occluders.push_back({a, b, c});
    s.occluders.push_back({a, c, e});
    light::Bake bk;
    std::string err;
    CHECK(light::bake(d, s, bk, err));
    if (!err.empty()) std::printf("  bake: %s\n", err.c_str());
    const std::vector<Face> pf = worldgeom::worldPatchFaces(d);
    bool all = !pf.empty();
    for (const Face& f : pf) all &= bk.find(f) != nullptr;
    CHECK(all);
    // luxel light at a point of a face
    auto luxel = [&](const Face& f, const Vec3& p, float* rgb, uint8_t* vis) {
        const light::FaceMap* m = bk.find(f);
        if (!m) return false;
        const int x = m->x + static_cast<int>((p[m->axisU] - m->minU) / bk.luxelSize);
        const int y = m->y + static_cast<int>((p[m->axisV] - m->minV) / bk.luxelSize);
        const size_t i = static_cast<size_t>(y) * bk.width + x;
        for (int k = 0; k < 3; k++) rgb[k] = bk.light[3 * i + k];
        *vis = bk.sunVis[static_cast<size_t>(2 * y) * (2 * bk.width) + 2 * x];
        return true;
    };
    Brush floor = d.ents[0].brushes[0];
    CHECK(floor.rebuild());
    const Face* top = nullptr;
    for (const Face& f : floor.faces)
        if (f.normal.z > 0.9) top = &f;
    Brush db = d.ents[1].brushes[0];
    CHECK(db.rebuild());
    const Face* doorTop = nullptr;
    for (const Face& f : db.faces)
        if (f.normal.z > 0.9) doorTop = &f;
    float open[3], shade[3], dl[3];
    uint8_t vOpen = 0, vShade = 0, vDoor = 0;
    CHECK(top && doorTop && luxel(*top, {0, -300, 0}, open, &vOpen) && luxel(*top, {-250, -250, 0}, shade, &vShade) &&
          luxel(*doorTop, {320, 0, 40}, dl, &vDoor));
    CHECK(vOpen == 255 && vShade == 0);  // the occluder's shadow
    // world floor: direct sun left to the game (excluded); the brush model's top keeps it
    CHECK(dl[0] > open[0] + 0.5f);
    const light::FaceMap* dm = bk.find(*doorTop);
    CHECK(dm && dm->primary == -1);
}

static void testWorldGeometry() {
    namespace fs = std::filesystem;
    const std::string trainer = std::string(kGame) + "/zone/english/trainer.ff";
    if (!fs::exists(trainer)) {
        std::printf("world geometry: skipped (no game install)\n");
        return;
    }
    std::string err;
    fs::path dir = fs::temp_directory_path() / "iw4r_geo_test";
    fs::create_directories(dir / "zone_source" / "xmodels");
    { std::ofstream(dir / "zone_source" / "xmodels" / "iw4r_geo_crate.xmodel_export") << cubeXModelExport("crate", "c:/art/crate_col.png"); }
    std::ifstream in(trainer, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Fastfile tz;
    CHECK(readFastfile(bytes, tz, err));
    auto lib = std::make_shared<matdef::Library>();
    matdef::scan(tz.zone, *lib);

    Document d;
    d.addBrush(makeBox({-512, -512, -16}, {512, 512, 0}, "wc/me_concrete"));
    d.ents[0].rawBlocks.push_back(kCurvePatch);
    Entity door;
    door.set("classname", "script_brushmodel");
    door.set("targetname", "door");
    door.brushes.push_back(makeBox({200, -32, 0}, {204, 32, 96}, "wc/me_concrete"));
    door.brushes.push_back(makeBox({196, -40, 96}, {208, 40, 100}, "clip"));  // collision only
    d.ents.push_back(door);
    Entity hidden;  // collision-only brush model: no surfaces
    hidden.set("classname", "script_brushmodel");
    hidden.set("origin", "0 300 0");
    hidden.brushes.push_back(makeBox({-16, 284, 0}, {16, 316, 32}, "clip"));
    d.ents.push_back(hidden);
    const char* modes[3] = {"", "box", "none"};
    for (int i = 0; i < 3; i++) {
        Sel s = d.addPointEntity("misc_model", {-200.0, -150.0 + 100 * i, 0});
        d.ents[s.ent].set("model", "iw4r_geo_crate");
        d.ents[s.ent].set("angles", "0 30 0");
        if (modes[i][0]) d.ents[s.ent].set("collision", modes[i]);
    }
    Sel st = d.addPointEntity("misc_model", {0, -300, 0});
    d.ents[st.ent].set("model", "com_barrel_benzin");  // stock: by name, no collision
    Sel named = d.addPointEntity("misc_model", {0, -400, 0});  // scripts can find it: stays an entity
    d.ents[named.ent].set("model", "iw4r_geo_crate");
    d.ents[named.ent].set("targetname", "named_crate");

    CompileOptions o = kNoBake;
    o.materials = lib;
    o.zoneSourceDir = (dir / "zone_source").string();
    std::vector<std::string> warnings;
    o.warnings = &warnings;
    std::vector<uint8_t> ff;
    CHECK(compileMapToFastfile(d, "mp_geo", ff, err, MapTarget::MP, o));
    if (!err.empty()) std::printf("  compile: %s\n", err.c_str());
    Fastfile zf;
    CHECK(readFastfile(ff, zf, err));
    zone::SimulatedZone sim;
    CHECK(zone::simulateLoad(zf.zone, sim, err));
    if (!err.empty()) std::printf("  simulateLoad (geometry): %s\n", err.c_str());
    auto* gw = static_cast<const iw4::GfxWorld*>(sim.find(IW4_GFXWORLD));
    auto* cm = static_cast<const iw4::clipMap_t*>(sim.find(IW4_CLIPMAP_MP));
    auto* me = static_cast<const iw4::MapEnts*>(sim.find(IW4_MAPENTS));
    auto* xm = static_cast<const iw4::XModel*>(sim.find(IW4_XMODEL));
    CHECK(gw && cm && me && xm);
    if (!gw || !cm || !me || !xm) return;

    // model collision surface: the cube's hull (12 triangles), barycentric s/t on its corners
    CHECK(xm->numCollSurfs == 1 && xm->collSurfs && xm->collLod == 0 && xm->contents == 1 && xm->lodInfo[0].dist >= 1e5f);
    if (xm->collSurfs) {
        const iw4::XModelCollSurf_s& c = xm->collSurfs[0];
        CHECK(c.numCollTris == 12 && c.contents == 1);
        bool bary = true;
        for (int t = 0; t < c.numCollTris; t++) {
            const iw4::XModelCollTri_s& r = c.collTris[t];
            // the triangle's corners lie on its plane with s, t in {0, 1}
            int corners = 0;
            for (int i = 0; i < 8; i++) {
                const float p[3] = {i & 1 ? 16.f : -16.f, i & 2 ? 16.f : -16.f, i & 4 ? 32.f : 0.f};
                const float pd = r.plane[0] * p[0] + r.plane[1] * p[1] + r.plane[2] * p[2] - r.plane[3];
                if (std::fabs(pd) > 0.01f) continue;
                const float sv = r.svec[0] * p[0] + r.svec[1] * p[1] + r.svec[2] * p[2] - r.svec[3];
                const float tv = r.tvec[0] * p[0] + r.tvec[1] * p[1] + r.tvec[2] * p[2] - r.tvec[3];
                if (sv > -0.01f && tv > -0.01f && sv + tv < 1.01f) corners++;
            }
            bary &= corners == 3;
        }
        CHECK(bary);
    }

    // brush models: GfxWorld models 1..2 after the world; surfaces after the static ones
    CHECK(gw->modelCount == 3 && cm->numSubModels == 3);
    const unsigned statics = gw->dpvs.staticSurfaceCount;
    CHECK(gw->models[0].surfaceCount == statics && gw->surfaceCount > statics);
    const iw4::GfxBrushModel& dm = gw->models[1];
    CHECK(dm.surfaceCount >= 1 && dm.startSurfIndex == statics);
    if (dm.surfaceCount && dm.startSurfIndex < gw->surfaceCount) {
        // origin = the door brushes' centre (202 0 50): vertices are local
        const iw4::GfxSurface& s = gw->dpvs.surfaces[dm.startSurfIndex];
        const iw4::GfxWorldVertex& v = gw->draw.vd.vertices[s.tris.firstVertex];
        CHECK(std::fabs(v.xyz[0]) <= 2.01f && std::fabs(v.xyz[1]) <= 32.01f && v.xyz[2] >= -50.01f && v.xyz[2] <= 46.01f);
        CHECK(s.laf.fields.primaryLightIndex == 0);
    }
    CHECK(gw->models[2].surfaceCount == 0 && gw->models[2].startSurfIndex == 0xFFFF);
    const std::string ents = me->entityString;
    CHECK(ents.find("\"model\" \"*1\"") != std::string::npos && ents.find("\"model\" \"*2\"") != std::string::npos);
    CHECK(ents.find("\"origin\" \"202 0 50\"") != std::string::npos && ents.find("misc_model") == std::string::npos);
    CHECK(ents.find("\"targetname\" \"named_crate\"") != std::string::npos && ents.find("\"classname\" \"script_model\"") != std::string::npos);
    // collision: cmodel 1's leaf lists its two brushes, relative to the origin
    const iw4::cmodel_t& c1 = cm->cmodels[1];
    CHECK(c1.leaf.leafBrushNode > 0 && std::fabs(c1.bounds.midPoint[0]) < 4 && std::fabs(c1.bounds.halfSize[0] - 7) < 0.01f);
    if (c1.leaf.leafBrushNode > 0) {
        const iw4::cLeafBrushNode_s& n = cm->leafbrushNodes[c1.leaf.leafBrushNode];
        CHECK(n.leafBrushCount == 2);
        if (n.leafBrushCount == 2) {
            const iw4::Bounds& bb = cm->brushBounds[n.data.leaf.brushes[0]];
            CHECK(std::fabs(bb.midPoint[0]) < 0.01f && std::fabs(bb.midPoint[2] + 2) < 0.01f);
        }
    }
    const iw4::cmodel_t& c2 = cm->cmodels[2];
    CHECK(std::fabs(c2.bounds.midPoint[1]) < 0.01f && std::fabs(c2.bounds.midPoint[2] - 16) < 0.01f);

    // static models: 4 draw insts (custom ones share one XModel), the stock one by name
    const iw4::GfxWorldDpvsStatic& v = gw->dpvs;
    CHECK(v.smodelCount == 4 && v.smodelInsts && v.smodelDrawInsts && v.smodelVisDataCount == 4);
    if (v.smodelCount == 4) {
        // by-name references (resolved by the game), one per distinct model, shared through its slot
        const iw4::XModel* r0 = v.smodelDrawInsts[0].model;
        CHECK(r0 && std::string(r0->name) == ",iw4r_geo_crate" && v.smodelDrawInsts[1].model == r0 && v.smodelDrawInsts[2].model == r0);
        CHECK(v.smodelDrawInsts[3].model && std::string(v.smodelDrawInsts[3].model->name) == ",com_barrel_benzin");
        const iw4::GfxStaticModelDrawInst& d0 = v.smodelDrawInsts[0];
        CHECK(d0.placement.origin[0] == -200 && d0.placement.scale == 1 && std::fabs(d0.placement.axis[0][1] - 0.5f) < 1e-5f &&
              d0.cullDist == 0);
        // world bounds of the rotated 32x32x32 cube: half-extent 16*(cos30 + sin30)
        CHECK(std::fabs(v.smodelInsts[0].bounds.halfSize[0] - 16 * (0.8660254f + 0.5f)) < 0.01f &&
              std::fabs(v.smodelInsts[0].lightingOrigin[2] - 16) < 0.01f);
    }
    const iw4::GfxAabbTree& tree = gw->aabbTrees[0].aabbTree[0];
    CHECK(tree.smodelIndexCount == 4 && tree.smodelIndexes && tree.smodelIndexes[3] == 3);
    // clipMap: the two crates with collision (not "none", not the stock one), one leaf node
    CHECK(cm->numStaticModels == 2 && cm->staticModelList && cm->smodelNodeCount == 1);
    if (cm->numStaticModels == 2) {
        const iw4::cStaticModel_s& s0 = cm->staticModelList[0];
        CHECK(s0.xmodel == v.smodelDrawInsts[0].model && cm->smodelNodes[0].childCount == 2 && cm->smodelNodes[0].firstChild == 0);
        // invScaledAxis[r][c] = axis[c][r] / scale
        CHECK(std::fabs(s0.invScaledAxis[1][0] - 0.5f) < 1e-5f && std::fabs(s0.invScaledAxis[0][1] + 0.5f) < 1e-5f);
    }
    // player clip: world brushes include the patch prisms and one clip brush per colliding crate
    int playerClip = 0;
    for (unsigned i = 0; i < cm->numBrushes; i++) playerClip += (cm->brushContents[i] & 0x30000) && !(cm->brushContents[i] & 1);
    CHECK(playerClip >= 2 + 1);  // 2 crates + the door's clip brush
    bool found = false;
    for (const std::string& w : warnings) found |= w.find("com_barrel_benzin") != std::string::npos;
    CHECK(found);
    // the patch is drawn: some world vertex sits on the curve's wall top (x 128, z 128)
    bool wall = false;
    for (unsigned i = 0; i < gw->draw.vertexCount; i++)
        wall |= std::fabs(gw->draw.vd.vertices[i].xyz[0] - 128) < 0.01f && std::fabs(gw->draw.vd.vertices[i].xyz[2] - 128) < 0.01f;
    CHECK(wall);
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

// Colour grading + SSAO settings: the baked table, artist LUT strips and the .volumetric lines the
// DLL reads.
static void testPostSettings() {
    using namespace light;
    // Neutral settings bake an identity table.
    GradeParams none;
    GradeLut id = bakeGradeLut(none, nullptr, 17);
    CHECK(id.size == 17 && id.rgb.size() == size_t(17) * 17 * 17 * 3);
    int worst = 0;
    for (int b = 0; b < 17; b++)
        for (int g = 0; g < 17; g++)
            for (int r = 0; r < 17; r++) {
                const uint8_t* p = &id.rgb[((size_t(b) * 17 + g) * 17 + r) * 3];
                const int want[3] = {int(std::lround(r * 255.0 / 16)), int(std::lround(g * 255.0 / 16)), int(std::lround(b * 255.0 / 16))};
                for (int k = 0; k < 3; k++) worst = std::max(worst, std::abs(p[k] - want[k]));
            }
    CHECK(worst <= 1);
    // Each knob moves mid grey the right way.
    const double grey[3] = {0.5, 0.5, 0.5}, red[3] = {0.8, 0.3, 0.2};
    double o[3];
    GradeParams p;
    p.exposure = 1;
    gradeColor(p, nullptr, grey, o);
    CHECK(o[0] > 0.65 && std::fabs(o[0] - o[2]) < 1e-6);
    p = {};
    p.temperature = 1;
    gradeColor(p, nullptr, grey, o);
    CHECK(o[0] > 0.52 && o[2] < 0.48);
    p = {};
    p.saturation = 0;
    gradeColor(p, nullptr, red, o);
    CHECK(std::fabs(o[0] - o[1]) < 1e-6 && std::fabs(o[1] - o[2]) < 1e-6);
    p = {};
    p.contrast = 2;
    const double dark[3] = {0.3, 0.3, 0.3};
    gradeColor(p, nullptr, dark, o);
    CHECK(std::fabs(o[0] - 0.1) < 1e-6);
    p = {};
    p.lift[2] = 0.2;
    const double black[3] = {0, 0, 0};
    gradeColor(p, nullptr, black, o);
    CHECK(o[0] == 0 && std::fabs(o[2] - 0.2) < 1e-6);

    // Artist strip LUT: a neutral one round-trips; a hand-made "swap red and blue" one is applied.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "iw4r_grade_test";
    fs::create_directories(dir / "zone_source" / "luts");
    std::string err;
    const std::string neutral = (dir / "neutral.png").string();
    CHECK(saveNeutralStripLut(neutral, 16, err));
    GradeLut art;
    CHECK(loadStripLut(neutral, art, err) && art.size == 16);
    gradeColor(none, &art, red, o);
    CHECK(std::fabs(o[0] - 0.8) < 0.01 && std::fabs(o[1] - 0.3) < 0.01 && std::fabs(o[2] - 0.2) < 0.01);
    {
        imageio::Rgba img;
        CHECK(imageio::load(neutral, img, err));
        for (size_t i = 0; i < img.pixels.size(); i += 4) std::swap(img.pixels[i], img.pixels[i + 2]);
        CHECK(imageio::savePng((dir / "zone_source" / "luts" / "swap.png").string(), img, err));
    }
    imageio::Rgba bad;
    bad.width = 30, bad.height = 7;
    bad.pixels.assign(30 * 7 * 4, 0);
    CHECK(imageio::savePng((dir / "bad.png").string(), bad, err));
    CHECK(!loadStripLut((dir / "bad.png").string(), art, err) && err.find("LUT strip") != std::string::npos);

    // Worldspawn keys -> Volumetric -> .volumetric text.
    Document d;
    d.path = (dir / "gradetest.map").string();
    d.ents[0].set("volumetric_ssao", "1");
    d.ents[0].set("volumetric_ssao_radius", "48");
    d.ents[0].set("volumetric_ssao_intensity", "1.25");
    d.ents[0].set("volumetric_grade", "1");
    d.ents[0].set("volumetric_grade_lut", "swap");
    d.ents[0].set("volumetric_grade_vignette", "0.4");
    d.ents[0].set("volumetric_grade_gain", "1 1 1");
    Volumetric v = readVolumetric(d);
    CHECK(v.enabled && v.ssao && v.ssaoRadius == 48 && v.ssaoIntensity == 1.25 && v.grade && v.vignette == 0.4);
    CHECK(v.gradeLut && v.gradeLut->size == 32 && v.gradeWarning.empty());
    if (v.gradeLut) {  // pure red in -> blue out (the swap LUT)
        const uint8_t* px = &v.gradeLut->rgb[(size_t(31)) * 3];  // r = 31, g = 0, b = 0
        CHECK(px[0] < 4 && px[1] < 4 && px[2] > 250);
    }
    CHECK(readVolumetric(d).gradeLut == v.gradeLut);  // cached
    const std::string cfg = volumetricConfig(d);
    CHECK(cfg.find("\nssao 1\n") != std::string::npos && cfg.find("\nssaoradius 48\n") != std::string::npos);
    CHECK(cfg.find("\ngrade 1\n") != std::string::npos && cfg.find("\nvignette 0.4\n") != std::string::npos);
    CHECK(cfg.find("\nlut 32\n") != std::string::npos);
    CHECK(cfg.find("\ncontact 0\n") != std::string::npos);  // off by default
    {
        Document c = d;
        c.ents[0].set("volumetric_contact", "1");
        c.ents[0].set("volumetric_contact_length", "1000");  // clamped to 512
        c.ents[0].set("volumetric_contact_intensity", "0.5");
        const Volumetric cv = readVolumetric(c);
        CHECK(cv.contact && cv.contactLength == 512 && cv.contactIntensity == 0.5 && cv.contactThickness == 24);
        const std::string cc = volumetricConfig(c);
        CHECK(cc.find("\ncontact 1\ncontactparm 0.5 512 24 2500\ncontactnear 20\n") != std::string::npos);
    }
    size_t rows = 0;
    for (size_t at = cfg.find("\nlutrow "); at != std::string::npos; at = cfg.find("\nlutrow ", at + 1)) rows++;
    CHECK(rows == 32 * 32);
    // A row: "lutrow b g " + 32 * 6 hex digits.
    const size_t r0 = cfg.find("\nlutrow 0 0 ");
    CHECK(r0 != std::string::npos && cfg.find('\n', r0 + 1) - (r0 + 1) == std::string("lutrow 0 0 ").size() + 32 * 6);
    // Missing LUT: grading still on (parameters only), with a warning.
    d.ents[0].set("volumetric_grade_lut", "nope");
    v = readVolumetric(d);
    CHECK(v.gradeLut && v.gradeWarning.find("not found") != std::string::npos);
    // Grading off: no table written, the rest of the file as before.
    d.ents[0].set("volumetric_grade", "0");
    d.ents[0].set("volumetric_ssao", "0");
    const std::string off = volumetricConfig(d);
    CHECK(off.find("\ngrade 0\n") != std::string::npos && off.find("lutrow") == std::string::npos && off.find("\nssao 0\n") != std::string::npos);

    // trigger_grade volumes: inherit worldspawn, override their keys; grading turns on for them alone.
    d.ents[0].set("volumetric_grade_lut", "");
    d.ents[0].set("volumetric_grade_saturation", "0.5");
    Entity tg;
    tg.set("classname", "trigger_grade");
    tg.set("targetname", "cellar");
    tg.set("grade_temperature", "-0.8");
    tg.set("blend", "32");
    tg.set("blend_time", "1.5");
    tg.set("priority", "2");
    tg.brushes.push_back(makeBox({0, 0, 0}, {256, 256, 128}, "trigger"));
    d.ents.push_back(tg);
    Entity tg2 = tg;
    tg2.set("targetname", "attic");
    tg2.set("priority", "-1");
    tg2.set("grade_saturation", "0");
    d.ents.push_back(tg2);
    v = readVolumetric(d);
    CHECK(v.grade && v.gradeLut && v.gradeVolumes.size() == 2);
    if (v.gradeVolumes.size() == 2) {
        CHECK(v.gradeVolumes[0].name == "attic" && v.gradeVolumes[1].name == "cellar");  // by priority
        const GradeVolume& c = v.gradeVolumes[1];
        CHECK(c.blend == 32 && c.blendTime == 1.5 && c.priority == 2 && c.brushes.size() == 1 && c.brushes[0].size() == 6);
        // worldspawn grade is off, so the volume inherits neutral settings, not the 0.5 saturation
        CHECK(c.params.temperature == -0.8 && c.params.saturation == 1 && c.lut && c.lut != v.gradeLut);
        // weights: centre of the box (64 inside the nearest wall: full), 16 inside (half), outside (none)
        const float mid[3] = {128, 128, 64}, edge[3] = {16, 128, 64}, out[3] = {-10, 128, 64};
        CHECK(gradevol::targetWeight(c.brushes, 32, mid) == 1.0f);
        CHECK(std::fabs(gradevol::targetWeight(c.brushes, 32, edge) - 0.5f) < 1e-4f);
        CHECK(gradevol::targetWeight(c.brushes, 32, out) == 0.0f);
        CHECK(gradevol::approach(0, 1, 2, 0.5f) == 0.25f && gradevol::approach(0.5f, 0, 0, 0.1f) == 0.0f);
        std::vector<uint8_t> a{0, 100, 200}, b{200, 100, 0};
        gradevol::blendInto(a, b, 0.5f);
        CHECK(a[0] == 100 && a[1] == 100 && a[2] == 100);
    }
    const std::string withVols = volumetricConfig(d);
    CHECK(withVols.find("\ngrade 1\n") != std::string::npos && withVols.find("\ngradevolume 0 32 1.5 0\n") != std::string::npos &&
          withVols.find("\ngradevolume 1 ") != std::string::npos);
    size_t volRows = 0, planes = 0;
    for (size_t at = withVols.find("\nvollutrow "); at != std::string::npos; at = withVols.find("\nvollutrow ", at + 1)) volRows++;
    for (size_t at = withVols.find("\nvolplane "); at != std::string::npos; at = withVols.find("\nvolplane ", at + 1)) planes++;
    CHECK(volRows == 2 * 32 * 32 && planes == 12);
    // With the worldspawn grade on, volumes inherit its settings.
    d.ents[0].set("volumetric_grade", "1");
    v = readVolumetric(d);
    CHECK(v.gradeVolumes.size() == 2 && v.gradeVolumes[1].params.saturation == 0.5 && v.gradeVolumes[0].params.saturation == 0);

    // Depth of field.
    d.ents[0].set("volumetric_dof", "1");
    d.ents[0].set("volumetric_dof_far_start", "800");
    d.ents[0].set("volumetric_dof_far_end", "300");  // before the start: clamped up to it
    d.ents[0].set("volumetric_dof_near_blur", "99");
    d.ents[0].set("volumetric_dof_auto", "1");
    v = readVolumetric(d);
    CHECK(v.dof && v.dofAuto && v.dofFarStart == 800 && v.dofFarEnd == 800 && v.dofNearBlur == 32 && v.dofWeapon == 24);
    const std::string dofCfg = volumetricConfig(d);
    CHECK(dofCfg.find("\ndof 1\ndofnear 0 0\ndoffar 800 800\ndofblur 32 4\ndofweapon 24\ndofauto 1\n") != std::string::npos);

    // Sun flare, glare, weather (+ trigger_weather shelters).
    d.ents[0].set("volumetric_flare", "1");
    d.ents[0].set("volumetric_flare_ghosts", "2");
    d.ents[0].set("volumetric_glare", "1");
    d.ents[0].set("volumetric_glare_radius", "200");  // clamped to 64
    d.ents[0].set("volumetric_weather", "2");
    d.ents[0].set("volumetric_weather_wind", "40 -10 0");
    Entity tw;
    tw.set("classname", "trigger_weather");
    tw.set("targetname", "porch");
    tw.set("weather_intensity", "0.25");
    tw.brushes.push_back(makeBox({0, 0, 0}, {128, 128, 96}, "trigger"));
    d.ents.push_back(tw);
    v = readVolumetric(d);
    CHECK(v.flare && v.flareGhosts == 2 && v.glare && v.glareRadius == 64 && v.weather == 2 && v.weatherWind[0] == 40 &&
          v.weatherWind[1] == -10 && v.weatherOpacity == 0.85);
    CHECK(v.weatherVolumes.size() == 1 && v.weatherVolumes[0].name == "porch" && v.weatherVolumes[0].intensity == 0.25 &&
          v.weatherVolumes[0].brushes.size() == 1 && v.weatherVolumes[0].brushes[0].size() == 6);
    const std::string fxCfg = volumetricConfig(d);
    CHECK(fxCfg.find("\nflare 1\nflareparm 1 1 2 0.5\n") != std::string::npos && fxCfg.find("\nglare 1\nglareparm 0.8 0.6 64 0.3\n") != std::string::npos);
    CHECK(fxCfg.find("\nweather 2\nweatherparm 1 1 1200\nweatherwind 40 -10\n") != std::string::npos &&
          fxCfg.find("\nwxvolume 0 64 0.5 0.25\nwxbrush 0\n") != std::string::npos);

    // Local light shafts: a point light, a spotlight aimed by target, one by angles, one left out, one shaft-only.
    auto addLight = [&](const char* origin, std::initializer_list<std::pair<const char*, const char*>> keys) {
        Entity l;
        l.set("classname", "light");
        l.set("origin", origin);
        for (const auto& kv : keys) l.set(kv.first, kv.second);
        d.ents.push_back(l);
    };
    addLight("0 0 100", {{"radius", "200"}, {"_color", "1 0.5 0.25"}, {"intensity", "2"}, {"volumetric_flicker", "0.5"}});
    addLight("100 0 100", {{"target", "spotaim"}, {"fov_outer", "30"}});
    addLight("100 0 0", {{"targetname", "spotaim"}, {"classname", "info_null"}});
    addLight("0 100 100", {{"fov_outer", "40"}, {"fov_inner", "10"}, {"angles", "0 90 0"}});
    addLight("0 200 100", {{"volumetric", "0"}});
    addLight("0 300 100", {{"volumetric_only", "1"}, {"volumetric_intensity", "3"}});
    const std::vector<PointLight> pls = readLights(d);
    CHECK(pls.size() == 5);
    if (pls.size() == 5) {
        CHECK(!pls[0].spot && pls[0].radius == 200 && pls[0].color.y == 1 && pls[0].flicker == 0.5);
        CHECK(pls[1].spot && std::fabs(pls[1].spotDir.z + 1) < 1e-9 && std::fabs(pls[1].cosOuter - std::cos(30 * 3.14159265358979 / 180)) < 1e-9 &&
              std::fabs(pls[1].cosInner - std::cos(24 * 3.14159265358979 / 180)) < 1e-9);
        CHECK(pls[2].spot && std::fabs(pls[2].spotDir.y - 1) < 1e-9 && std::fabs(pls[2].cosInner - std::cos(10 * 3.14159265358979 / 180)) < 1e-9);
        CHECK(!pls[3].shafts && pls[4].bake == false && pls[4].shaftScale == 3);
    }
    v = readVolumetric(d);
    CHECK(!v.lights && v.volLights.empty());  // off until volumetric_lights
    d.ents[0].set("volumetric_lights", "1");
    d.ents[0].set("volumetric_lights_max", "40");  // clamped to 16
    v = readVolumetric(d);
    CHECK(v.lights && v.volLights.size() == 4 && v.lightsMax == 16 && v.volLights[3].color.x == 3);
    const std::string lCfg = volumetricConfig(d);
    CHECK(lCfg.find("\nlights 1\nlightsparm 1 0.8 16 4000\n") != std::string::npos &&
          lCfg.find("\nllight 0.00 0.00 100.00 2.0000 1.0000 0.5000 200 0.00000 0.00000 0.00000 -2.00000 -2.00000 0.5\n") != std::string::npos &&
          lCfg.find("\nllight 100.00 0.00 100.00 1.0000 1.0000 1.0000 300 0.00000 0.00000 -1.00000 0.86603 ") != std::string::npos);
    fs::remove_all(dir);
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
    testBrushTriggers();
    testZoneCompileSp();
    testPathNodes();
    testEffects();
    testZoneWriterModel();
    testRebuildStockRawfileZones();
    testClipMapCompile();
    testLargeClipMapTree();
    testGfxWorldCompile();
    testLightingBake();
    testPostSettings();
    testCustomMapScript();
    testWeaponPackLevel();
    testSurfaceTypes();
    testShapes();
    testPenOutlines();
    testTerrain();
    testFxPreview();
    testSkeletonConvert();
    testZoneSourceModel();
    testPatches();
    testWorldGeometry();
    testGeometryBake();
    testSkyCompile();
    testMaterialEmbed();
    testDumpFastfile();
    testImportFastfile();
    std::printf(failures ? "%d check(s) FAILED\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
