// iw4r_zone: command-line utilities for unsigned IW4 fastfiles.
//
//   iw4r_zone info <in.ff>            header, prefix bytes, inflated size, string/asset counts
//   iw4r_zone rebuild <in.ff> <out.ff>   read then re-write, verifying the payload round-trips
//   iw4r_zone compile <map.map> <out.ff> compile a map, then verify it with the loader simulator
//   iw4r_zone verify <in.ff>             load a zone through the offline port of the game loader
//   iw4r_zone dump <in.ff> [out-dir]     inflated zone + report.txt + extracted rawfiles/entities
//
// rebuild never touches the input or any game file; it writes only <out.ff>, so it's safe to
// point at a copy of a zone and compare the result, or drop the rebuilt file in to test loading.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "fastfile.h"
#include "mapio.h"
#include "zone/iw4.h"
#include "zone/zonebuild.h"
#include "light/lightbake.h"
#include <chrono>
#include "zone/mapimport.h"
#include "live/liveimport.h"
#include "zone/zonedump.h"
#include "zone/zonesim.h"
#include "zone/matdefs.h"
#include "zone/efx.h"
#include "brush.h"
#include <map>

namespace {
bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    out.resize(static_cast<size_t>(f.tellg()));
    f.seekg(0);
    return out.empty() || static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), out.size()));
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
    return static_cast<bool>(f);
}

std::string prefixHex(const uint8_t p[9]) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02x %02x %02x %02x %02x %02x %02x %02x %02x", p[0], p[1], p[2], p[3], p[4],
                  p[5], p[6], p[7], p[8]);
    return buf;
}

int info(const std::string& path) {
    std::vector<uint8_t> file;
    if (!readFile(path, file)) {
        std::printf("error: can't read %s\n", path.c_str());
        return 1;
    }
    FastfileKind kind = classifyFastfile(file.data(), file.size());
    const char* kindName = kind == FastfileKind::Unsigned ? "unsigned (IWffu100)"
                           : kind == FastfileKind::Signed ? "signed (IWffs100)"
                                                          : "not a fastfile";
    std::printf("file:      %s\n", path.c_str());
    std::printf("size:      %zu bytes\n", file.size());
    std::printf("kind:      %s\n", kindName);
    if (kind != FastfileKind::Unsigned && kind != FastfileKind::Signed) return 1;

    Fastfile ff;
    std::string err;
    if (!readFastfile(file, ff, err)) {
        std::printf("read failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("prefix[9]: %s\n", prefixHex(ff.prefix).c_str());
    std::printf("inflated:  %zu bytes\n", ff.zone.size());
    ZoneHeader h;
    if (readZoneHeader(ff.zone, h))
        std::printf("zone:      %u strings, %u assets\n", h.stringCount, h.assetCount);
    else
        std::printf("zone:      header didn't validate (unexpected layout)\n");
    return 0;
}

int rebuild(const std::string& in, const std::string& out) {
    std::vector<uint8_t> file;
    if (!readFile(in, file)) {
        std::printf("error: can't read %s\n", in.c_str());
        return 1;
    }
    Fastfile ff;
    std::string err;
    if (!readFastfile(file, ff, err)) {
        std::printf("read failed: %s\n", err.c_str());
        return 1;
    }
    std::vector<uint8_t> rebuilt;
    if (!writeFastfile(ff, rebuilt, err)) {
        std::printf("write failed: %s\n", err.c_str());
        return 1;
    }
    // Verify the payload survives a full read/write/read cycle before trusting the file.
    Fastfile back;
    if (!readFastfile(rebuilt, back, err)) {
        std::printf("verify failed: rebuilt file won't read back: %s\n", err.c_str());
        return 1;
    }
    if (back.zone != ff.zone) {
        std::printf("verify FAILED: inflated payload differs after round-trip\n");
        return 2;
    }
    bool prefixKept = std::equal(ff.prefix, ff.prefix + 9, back.prefix);
    if (!writeFile(out, rebuilt)) {
        std::printf("error: can't write %s\n", out.c_str());
        return 1;
    }
    std::printf("rebuilt %s -> %s\n", in.c_str(), out.c_str());
    std::printf("  payload: %zu bytes, identical after round-trip: yes\n", ff.zone.size());
    std::printf("  prefix preserved: %s\n", prefixKept ? "yes" : "no");
    std::printf("  size: %zu -> %zu bytes (recompressed)\n", file.size(), rebuilt.size());
    return 0;
}
// Loads the inflated zone the way the game does and prints what came out.
bool verifyZone(const std::vector<uint8_t>& zoneImage) {
    zone::SimulatedZone sim;
    std::string err;
    if (!zone::simulateLoad(zoneImage, sim, err)) {
        std::printf("  loader simulation FAILED: %s\n", err.c_str());
        return false;
    }
    std::printf("  loader simulation: ok, %zu assets\n%s", sim.assets.size(), zone::describeSimulatedZone(sim).c_str());
    return true;
}

// Bakes the map and writes the compiled lighting data as raw files for pictures (tools/lightviz.py):
// each lightmap page (secondary = T over B, BGRA; primary = L8 mask), each reflection probe (the
// stock cube bytes), light grid samples on a line through the map, and info.txt (atlas, pages,
// faces with their rect / page / primary light / tangent frame).
int lightViz(const std::string& mapPath, const std::string& outDir, const char* luxel, const char* pages) {
    Document doc;
    std::string err;
    if (!loadMap(mapPath, doc, err)) {
        std::printf("map load failed: %s\n", err.c_str());
        return 1;
    }
    light::Settings s;
    if (luxel) s.luxelSize = std::atof(luxel);
    if (pages) s.maxPages = std::atoi(pages);
    light::Bake b;
    if (!light::bake(doc, s, b, err)) {
        std::printf("bake failed: %s\n", err.c_str());
        return 1;
    }
    gfx::GfxWorldData gw;
    if (!gfx::buildGfxWorldData(doc, gw, err, &b, true, nullptr)) {
        std::printf("gfx world failed: %s\n", err.c_str());
        return 1;
    }
    namespace fs = std::filesystem;
    fs::create_directories(outDir);
    auto raw = [&](const std::string& name, const std::vector<uint8_t>& data) {
        std::ofstream(fs::path(outDir) / name, std::ios::binary).write(reinterpret_cast<const char*>(data.data()),
                                                                        static_cast<std::streamsize>(data.size()));
    };
    std::ofstream info(fs::path(outDir) / "info.txt");
    info << "atlas " << b.width << " " << b.height << " luxel " << b.luxelSize << " device " << b.device << "\n";
    info << "sun " << b.env.sunDir.x << " " << b.env.sunDir.y << " " << b.env.sunDir.z << " hasSun " << b.env.hasSun << "\n";
    for (size_t p = 0; p < gw.lightmapPrimaries.size(); p++) {
        raw("page" + std::to_string(p) + "_secondary.bin", gw.lightmapSecondaries[p].pixels);
        raw("page" + std::to_string(p) + "_primary.bin", gw.lightmapPrimaries[p].pixels);
        info << "page " << p << " " << gw.lightmapSecondaries[p].width << " " << gw.lightmapSecondaries[p].height / 2 << "\n";
    }
    for (size_t i = 0; i < gw.reflectionProbes.size(); i++) {
        raw("probe" + std::to_string(i) + ".bin", gw.reflectionProbes[i].pixels);
        info << "probe " << i << " " << gw.probeOrigins[i][0] << " " << gw.probeOrigins[i][1] << " " << gw.probeOrigins[i][2] << " "
             << gw.reflectionProbes[i].width << "\n";
    }
    for (size_t i = 0; i < b.primaries.size(); i++)
        info << "primarylight " << i << " " << b.primaries[i].origin.x << " " << b.primaries[i].origin.y << " " << b.primaries[i].origin.z << "\n";
    for (const Brush& br : doc.ents[0].brushes) {
        Brush bb = br;
        if (!bb.rebuild()) continue;
        for (const Face& f : bb.faces) {
            const light::FaceMap* m = b.find(f);
            if (!m) continue;
            const Vec3 t = gfx::texGradientOf(f), bn = cross(f.normal, t) * gfx::binormalSignOf(f);
            info << "face " << m->x << " " << m->y << " " << m->w << " " << m->h << " page " << m->page << " primary " << m->primary
                 << " n " << f.normal.x << " " << f.normal.y << " " << f.normal.z << " t " << t.x << " " << t.y << " " << t.z << " b " << bn.x
                 << " " << bn.y << " " << bn.z << " axes " << m->axisU << " " << m->axisV << " min " << m->minU << " " << m->minV << " d " << f.dist
                 << "\n";
        }
    }
    // Grid samples at the points listed in the map's "info_null" entities named "viz" (else none).
    std::vector<Vec3> pts;
    for (const Entity& e : doc.ents) {
        const std::string* tn = e.find("targetname");
        Vec3 o;
        if (tn && *tn == "viz" && e.origin(o)) pts.push_back(o);
    }
    const std::vector<light::GridSample> gs = light::sampleGrid(b, pts);
    const auto& gd = light::gridDirections();
    for (size_t i = 0; i < gs.size(); i++) {
        info << "grid " << pts[i].x << " " << pts[i].y << " " << pts[i].z << " primary " << gs[i].primary;
        for (size_t k = 0; k < 56; k++) info << " " << gs[i].dir[k].x << " " << gs[i].dir[k].y << " " << gs[i].dir[k].z;
        info << "\n";
    }
    for (size_t k = 0; k < 56; k++) info << "griddir " << gd[k].x << " " << gd[k].y << " " << gd[k].z << "\n";
    std::printf("wrote %s: %zu page(s), %zu probe(s), %zu primary light(s), %zu grid point(s)\n", outDir.c_str(), gw.lightmapPrimaries.size(),
                gw.reflectionProbes.size(), b.primaries.size(), gs.size());
    return 0;
}

// Bakes the map on the CPU and on the GPU and compares them: timings, and how far apart the
// results are (sun mask exactly; light per luxel relative to the CPU's, both noisy).
int bakeBench(const std::string& mapPath, const char* luxel) {
    Document doc;
    std::string err;
    if (!loadMap(mapPath, doc, err)) {
        std::printf("map load failed: %s\n", err.c_str());
        return 1;
    }
    light::Settings s;
    if (luxel) s.luxelSize = std::atof(luxel);
    light::Bake cpu, gpu;
    s.gpu = false;
    auto t0 = std::chrono::steady_clock::now();
    if (!light::bake(doc, s, cpu, err)) {
        std::printf("cpu bake failed: %s\n", err.c_str());
        return 1;
    }
    const double tc = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    s.gpu = true;
    t0 = std::chrono::steady_clock::now();
    if (!light::bake(doc, s, gpu, err)) {
        std::printf("gpu bake failed: %s\n", err.c_str());
        return 1;
    }
    const double tg = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("atlas %dx%d at %g units/luxel, %zu faces\n", cpu.width, cpu.height, cpu.luxelSize, cpu.faces.size());
    std::printf("cpu: %.2fs on %s\n", tc, cpu.device.c_str());
    std::printf("gpu: %.2fs on %s%s%s\n", tg, gpu.device.c_str(), gpu.gpuNote.empty() ? "" : " - GPU not used: ", gpu.gpuNote.c_str());
    if (gpu.width != cpu.width || gpu.height != cpu.height || gpu.light.size() != cpu.light.size()) {
        std::printf("FAIL: different atlases\n");
        return 1;
    }
    size_t sunDiff = 0;
    for (size_t i = 0; i < cpu.sunVis.size(); i++) sunDiff += cpu.sunVis[i] != gpu.sunVis[i];
    double sc = 0, sg = 0, sad = 0;
    for (size_t i = 0; i < cpu.light.size(); i++) sc += cpu.light[i], sg += gpu.light[i], sad += std::fabs(cpu.light[i] - gpu.light[i]);
    const double meanRel = sc > 0 ? (sg - sc) / sc : 0, madRel = sc > 0 ? sad / sc : 0;
    std::printf("sun mask: %zu of %zu samples differ (%.3f%%)\n", sunDiff, cpu.sunVis.size(), 100.0 * sunDiff / std::max<size_t>(1, cpu.sunVis.size()));
    std::printf("light: total %+.2f%% vs cpu, mean abs difference %.2f%% of the mean (sampling noise)\n", 100 * meanRel, 100 * madRel);
    std::printf("speed-up: %.1fx\n", tg > 0 ? tc / tg : 0.0);
    return 0;
}

int verify(const std::string& path) {
    std::vector<uint8_t> file;
    if (!readFile(path, file)) {
        std::printf("error: can't read %s\n", path.c_str());
        return 1;
    }
    Fastfile ff;
    std::string err;
    if (!readFastfile(file, ff, err)) {
        std::printf("read failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("verify %s\n", path.c_str());
    return verifyZone(ff.zone) ? 0 : 2;
}

// Game the live* commands read / compile targets: nullptr = default (live: whichever of iw4mp/iw4sp
// is running; compile: MP). --mp / --sp force one.
const char* gLiveGame = nullptr;
bool gCpuBake = false;  // --cpubake: light bake on the CPU only

int compile(const std::string& mapPath, const std::string& outPath) {
    const MapTarget target = gLiveGame && std::string(gLiveGame) == "iw4sp.exe" ? MapTarget::SP : MapTarget::MP;
    Document doc;
    std::string err;
    if (!loadMap(mapPath, doc, err)) {
        std::printf("map load failed: %s\n", err.c_str());
        return 1;
    }
    // Zone/base name is the output file stem (e.g. mp_test.ff -> "mp_test").
    std::string mapName = std::filesystem::path(outPath).stem().string();
    std::vector<uint8_t> ff;
    CompileOptions options;
    if (const char* la = std::getenv("LOCALAPPDATA")) options.cacheDir = std::string(la) + "\\iw4-radiant";
    // Game folder (weapon packs): IW4R_GAME, else the default Steam install.
    const char* game = std::getenv("IW4R_GAME");
    options.gameDir = game ? game : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Call of Duty Modern Warfare 2";
    const std::string* pack = doc.ents.empty() ? nullptr : doc.ents[0].find("weaponpack");
    if (pack && !pack->empty() && target == MapTarget::SP) options.addonLevel = weaponPackLevel(mapName);
    if (readMapScript(mapPath, options.mapScript))
        std::printf("map script: %s\n", mapScriptPath(mapPath).c_str());
    options.zoneSourceDir = zoneSourcePath(mapPath);
    std::vector<std::string> warnings;
    options.warnings = &warnings;
    options.light.gpu = !gCpuBake;
    bool compiled = compileMapToFastfile(doc, mapName, ff, err, target, options);
    for (const std::string& w : warnings) std::printf("warning: %s\n", w.c_str());
    if (!compiled) {
        std::printf("compile failed: %s\n", err.c_str());
        return 1;
    }
    Fastfile back;
    if (!readFastfile(ff, back, err)) {
        std::printf("internal error: compiled zone won't read back: %s\n", err.c_str());
        return 1;
    }
    std::printf("compiled %s (zone name %s)\n", mapPath.c_str(), mapName.c_str());
    if (!verifyZone(back.zone)) return 2;  // never write a zone the loader would reject
    {  // sun-shaft settings for iw4r_mapload.dll, next to the zone
        std::filesystem::path vp = std::filesystem::path(outPath).replace_extension(".volumetric");
        std::ofstream(vp, std::ios::trunc) << light::volumetricConfig(doc);
    }
    if (!writeFile(outPath, ff)) {
        std::printf("error: can't write %s\n", outPath.c_str());
        return 1;
    }
    if (!options.addonLevel.empty()) {
        std::string perr;
        if (deployWeaponPack(options.gameDir, std::filesystem::path(outPath).parent_path().string(), *pack, mapName, perr))
            std::printf("  weapon pack %s -> %s.ff; play with: devmap %s\n", pack->c_str(), options.addonLevel.c_str(),
                        options.addonLevel.c_str());
        else
            std::printf("  weapon pack: %s\n", perr.c_str());
    }
    std::printf("  wrote %s (%zu bytes): ComWorld, FxWorld, GfxWorld, %s, clipMap (+MapEnts, %zu entities)\n",
                outPath.c_str(), ff.size(), target == MapTarget::SP ? "GameWorldSp" : "GameWorldMp",
                doc.ents.size());
    std::printf("  not yet in the zone: non-world/unknown materials (by name), stock models' geometry\n"
                "  (static models using them are by-name references, no collision)\n");
    return 0;
}
int importMap(const std::string& in, const std::string& outMap) {
    Document doc;
    mapimport::ImportStats st;
    std::string err;
    auto t0 = std::chrono::steady_clock::now();
    if (!mapimport::importFastfile(in, doc, st, err)) {
        std::printf("import failed: %s\n", err.c_str());
        return 1;
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("imported %s in %.1fs (%s)\n", in.c_str(), secs, st.partial ? "stock zone, per-asset" : "whole zone");
    std::printf("  entities: %zu\n  brushes:  %zu (%zu skipped)\n", st.entities, st.brushes, st.brushesSkipped);
    std::printf("  render:   %zu surfaces (%zu with a material), %zu triangles\n", st.surfaces, st.surfacesTextured,
                st.triangles);
    if (!st.notes.empty()) std::printf("  notes:    %s\n", st.notes.c_str());
    if (doc.refMesh) {
        std::vector<RefMesh::Batch> top = doc.refMesh->batches;
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.indexCount > b.indexCount; });
        std::printf("  render materials: %zu (%zu with a colorMap image), largest:", top.size(), doc.refMesh->materialImages.size());
        for (size_t i = 0; i < top.size() && i < 6; i++)
            std::printf(" %s(%u)", top[i].material.empty() ? "<none>" : top[i].material.c_str(), top[i].indexCount / 3);
        std::printf("\n");
    }
    if (!outMap.empty()) {
        if (!saveMap(outMap, doc, err)) {
            std::printf("save failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("  wrote %s (brushes + entities; the render mesh is reference-only)\n", outMap.c_str());
    }
    return 0;
}

int liveDump(const std::string& outMap) {
    Document doc;
    live::LiveStats st;
    std::string err;
    auto t0 = std::chrono::steady_clock::now();
    if (!live::importFromProcess(doc, st, err, gLiveGame)) {
        std::printf("live dump failed: %s\n", err.c_str());
        return 1;
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("dumped %s from %s in %.1fs\n", st.mapName.c_str(), st.game.c_str(), secs);
    std::printf("  models:    %zu instances (%zu unique, %zu skipped)\n", st.models, st.uniqueModels, st.modelsSkipped);
    std::printf("  geometry:  %zu surfaces, %zu triangles\n", st.surfaces, st.triangles);
    std::printf("  materials: %zu (%zu with a colorMap image)\n", st.materials, st.images);
    if (doc.refMesh) {
        std::vector<RefMesh::Batch> top = doc.refMesh->batches;
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.indexCount > b.indexCount; });
        std::printf("  largest:  ");
        for (size_t i = 0; i < top.size() && i < 6; i++)
            std::printf(" %s(%u)", top[i].material.empty() ? "<none>" : top[i].material.c_str(), top[i].indexCount / 3);
        std::printf("\n");
    }
    if (!outMap.empty()) {
        if (!saveMap(outMap, doc, err)) {
            std::printf("save failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("  wrote %s (entities only; the models are reference geometry, not brushes)\n", outMap.c_str());
    }
    return 0;
}

int liveModels(int argc, char** argv) {
    std::vector<std::string> names(argv, argv + argc);
    live::LiveModelLibrary lib;
    live::LiveModelStats st;
    std::string err;
    if (!live::loadLiveModels(names, lib, st, err, gLiveGame)) {
        std::printf("live models failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("[%s] found %zu of %zu (%zu surfaces, %zu triangles)\n", st.game.c_str(), st.found, st.requested, st.surfaces, st.triangles);
    for (const auto& [key, m] : lib) {
        std::printf("  %s: %zu surfaces, bounds (%.0f %.0f %.0f)-(%.0f %.0f %.0f)", m->name.c_str(), m->surfs.size(),
                    m->bounds.mins.x, m->bounds.mins.y, m->bounds.mins.z, m->bounds.maxs.x, m->bounds.maxs.y,
                    m->bounds.maxs.z);
        if (!m->surfs.empty()) std::printf(", first material %s -> %s", m->surfs[0].material.c_str(), m->surfs[0].image.c_str());
        std::printf("\n");
    }
    for (const std::string& n : st.missing) std::printf("  missing: %s\n", n.c_str());
    return 0;
}

std::string defaultCacheDir() {
    const char* la = std::getenv("LOCALAPPDATA");
    return la ? std::string(la) + "\\iw4-radiant" : std::string();
}

// matdefs <zone.ff|zone-folder>... : scans zones into the material-definition library (the cache
// the compiler and editor use), or with "show <name>" prints one definition.
int matDefs(int argc, char** argv) {
    const std::string cache = defaultCacheDir() + "\\materialdefs.bin";
    matdef::Library lib;
    matdef::load(cache, lib);
    if (argc >= 2 && std::string(argv[0]) == "show") {
        auto it = lib.find(matdef::key(argv[1]));
        if (it == lib.end()) {
            std::printf("%s: not in the library (%zu materials)\n", argv[1], lib.size());
            return 1;
        }
        const matdef::Def& d = it->second;
        std::printf("%s: techset %s, %zu textures, %zu constants, %zu state bits\n", d.name.c_str(),
                    matdef::techsetFor(d).c_str(), d.textures.size(), d.constants.size() / 32, d.stateBits.size() / 8);
        for (const matdef::Texture& t : d.textures)
            std::printf("  %c..%c sem %d sampler 0x%02x image %s\n", t.def[4], t.def[5], t.def[7], t.def[6],
                        t.image.empty() ? "<unknown>" : t.image.c_str());
        return 0;
    }
    std::vector<std::filesystem::path> files;
    for (int i = 0; i < argc; i++) {
        std::filesystem::path p(argv[i]);
        if (std::filesystem::is_directory(p)) {
            for (const auto& e : std::filesystem::directory_iterator(p))
                if (e.path().extension() == ".ff") files.push_back(e.path());
        } else {
            files.push_back(p);
        }
    }
    size_t before = lib.size();
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Fastfile zf;
        std::string err;
        if (!readFastfile(bytes, zf, err)) {
            std::printf("  %s: %s\n", f.filename().string().c_str(), err.c_str());
            continue;
        }
        size_t n = lib.size();
        matdef::scan(zf.zone, lib);
        std::printf("  %s: +%zu\n", f.filename().string().c_str(), lib.size() - n);
    }
    std::printf("%zu world materials (%zu new) -> %s\n", lib.size(), lib.size() - before, cache.c_str());
    return matdef::save(cache, lib) ? 0 : 1;
}

// refprobe <zone.ff> <ref>... : loads the zone's leading assets the sim supports, then dumps the
// 32 bytes each back-reference points to (as hex and as floats).
int refProbe(int argc, char** argv) {
    std::ifstream in(argv[0], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Fastfile zf;
    std::string err;
    if (!readFastfile(bytes, zf, err)) { std::printf("%s\n", err.c_str()); return 1; }
    zone::SimulatedZone sim;
    size_t loaded = 0;
    std::string stopped;
    zone::simulatePrefix(zf.zone, sim, loaded, stopped);
    std::printf("loaded %zu assets; VIRTUAL used 0x%x; stopped: %s\n", loaded, sim.blockUsed[3], stopped.c_str());
    for (int i = 1; i < argc; i++) {
        uint64_t ref = std::strtoull(argv[i], nullptr, 16);
        const uint8_t* p = sim.resolve(ref);
        if (!p) { std::printf("  %s: not loaded\n", argv[i]); continue; }
        float f[4];
        std::memcpy(f, p, 16);
        std::printf("  %s: %g %g %g %g |", argv[i], f[0], f[1], f[2], f[3]);
        for (int k = 0; k < 32; k++) std::printf(" %02x", p[k]);
        std::printf("\n");
    }
    return 0;
}

// modelprobe <zone.ff> : prefix-loads a stock zone and prints the XModels it reaches.
// fxprobe <zone.ff> [name-substring] [--samples]: loads the zone's leading assets the sim supports
// and prints each effect (FxEffectDef/FxElemDef, x64 layout) and the materials its sprites use.
namespace {
const char* matTechset(const uint8_t* m) {
    const uint8_t* ts;
    std::memcpy(&ts, m + 88, 8);
    if (!ts) return "-";
    const char* n;
    std::memcpy(&n, ts, 8);
    return n ? n : "?";
}
void printMaterial(const uint8_t* m) {
    const char* name;
    std::memcpy(&name, m, 8);
    std::printf("      material %s techset %s sortKey %u gameFlags 0x%02x atlas %ux%u stateFlags 0x%02x camRegion %u tex %u const %u sb %u\n",
                name ? name : "?", matTechset(m), m[9], m[8], m[10], m[11], m[83], m[84], m[80], m[81], m[82]);
    std::printf("        stateBitsEntry:");
    for (int i = 0; i < 48; i++) std::printf(" %d", static_cast<int8_t>(m[32 + i]));
    std::printf("\n");
    const uint8_t* sb;
    std::memcpy(&sb, m + 112, 8);
    for (int i = 0; sb && i < m[82]; i++) {
        uint32_t a, b;
        std::memcpy(&a, sb + 8 * i, 4);
        std::memcpy(&b, sb + 8 * i + 4, 4);
        std::printf("        stateBits[%d] %08x %08x\n", i, a, b);
    }
    const uint8_t* tt;
    std::memcpy(&tt, m + 96, 8);
    for (int i = 0; tt && i < m[80]; i++) {
        const uint8_t* img;
        std::memcpy(&img, tt + 16 * i + 8, 8);
        const char* in = nullptr;
        if (img) std::memcpy(&in, img + 32, 8);
        uint32_t hash;
        std::memcpy(&hash, tt + 16 * i, 4);
        std::printf("        texture hash %08x %c..%c sampler 0x%02x semantic %u image %s\n", hash, tt[16 * i + 4], tt[16 * i + 5],
                    tt[16 * i + 6], tt[16 * i + 7], in ? in : "-");
    }
    const uint8_t* ct;
    std::memcpy(&ct, m + 104, 8);
    for (int i = 0; ct && i < m[81]; i++) {
        float v[4];
        std::memcpy(v, ct + 32 * i + 16, 16);
        std::printf("        constant %.12s = %g %g %g %g\n", reinterpret_cast<const char*>(ct + 32 * i + 4), v[0], v[1], v[2], v[3]);
    }
}
}  // namespace

int fxProbe(int argc, char** argv) {
    std::ifstream in(argv[0], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Fastfile zf;
    std::string err;
    if (!readFastfile(bytes, zf, err)) { std::printf("%s\n", err.c_str()); return 1; }
    if (argc >= 2 && std::string(argv[1]) == "--loops") {
        // Raw scan for looping FxElemDefs (the sim stops early in most stock zones): the spawn
        // count at +8 of each element, after a plausible interval at +4 and life span at +48.
        std::map<int32_t, int> counts;
        const std::vector<uint8_t>& z = zf.zone;
        auto rd = [&](size_t o) { int32_t v; std::memcpy(&v, z.data() + o, 4); return v; };
        for (size_t o = 0; o + 288 <= z.size(); o += 4) {
            const int32_t flags = rd(o), interval = rd(o + 4), count = rd(o + 8), life = rd(o + 48);
            if (interval < 10 || interval > 20000 || life < 50 || life > 60000) continue;
            float cull, rangeLo;
            std::memcpy(&rangeLo, z.data() + o + 12, 4);
            std::memcpy(&cull, z.data() + o + 36, 4);
            if (!(cull >= 0 && cull < 100000) || !(rangeLo >= 0 && rangeLo < 100000)) continue;
            if ((static_cast<uint32_t>(flags) & 0xFF000000u) == 0 || (static_cast<uint32_t>(flags) >> 28) > 7) continue;
            if (count == 0x7FFFFFFF || count == 0 || (count > 0 && count < 1000)) counts[count]++;
        }
        (void)counts;
        // Exact: an inline FxEffectDef is 40 bytes (name -1, flags, totalSize, msecLoopingLife,
        // looping/oneShot/emission counts, elemDefs -1), then its name, then its elemDefs (288 each).
        int shown = 0;
        for (size_t o = 0; o + 48 <= z.size() && shown < 40; o += 1) {
            uint64_t np, ep;
            std::memcpy(&np, z.data() + o, 8);
            std::memcpy(&ep, z.data() + o + 32, 8);
            if (np != ~0ull || ep != ~0ull) continue;
            const int32_t loop = rd(o + 20), one = rd(o + 24), emis = rd(o + 28);
            if (loop <= 0 || loop > 32 || one < 0 || one > 64 || emis < 0 || emis > 64) continue;
            const char* nm = reinterpret_cast<const char*>(z.data() + o + 40);
            size_t len = strnlen(nm, 128);
            if (len < 4 || len >= 128 || std::memchr(nm, '/', len) == nullptr) continue;
            bool ok = true;
            for (size_t k = 0; k < len; k++) ok &= nm[k] >= 0x20 && nm[k] < 0x7f;
            if (!ok) continue;
            const size_t elems = o + 40 + len + 1;  // align 4
            const size_t e0 = (elems + 3) & ~size_t(3);
            std::printf("%-44s msecLoopingLife %d looping %d:", nm, rd(o + 16), loop);
            for (int k = 0; k < loop && e0 + 288 * (k + 1) <= z.size(); k++)
                std::printf(" [interval %d count %d]", rd(e0 + 288 * k + 4), rd(e0 + 288 * k + 8));
            std::printf("\n");
            shown++;
        }
        return 0;
    }
    zone::SimulatedZone sim;
    size_t loaded = 0;
    std::string stopped;
    if (!zone::simulatePrefix(zf.zone, sim, loaded, stopped)) {
        std::string e;  // a complete zone (our own): load it fully
        zone::simulateLoad(zf.zone, sim, e);
    }
    std::string filter;
    bool samples = false, mats = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--samples") samples = true;
        else if (a == "--materials") mats = true;
        else filter = a;
    }
    std::printf("loaded %zu assets; stopped: %s\n", loaded, stopped.c_str());
    if (mats) {
        for (const auto& a : sim.assets)
            if (a.type == 5 && !a.reference) {
                const uint8_t* m = static_cast<const uint8_t*>(a.header);
                const char* n;
                std::memcpy(&n, m, 8);
                if (n && std::string(n).find(filter) != std::string::npos) printMaterial(m);
            }
        return 0;
    }
    static const char* kTypes[] = {"billboard", "oriented", "tail", "trail", "cloud", "sparkcloud", "sparkfountain",
                                   "model", "omnilight", "spotlight", "sound", "decal", "runner"};
    for (const auto& a : sim.assets) {
        if (a.type != 30 || a.reference) continue;
        const uint8_t* fx = static_cast<const uint8_t*>(a.header);
        const char* name;
        std::memcpy(&name, fx, 8);
        if (!name || std::string(name).find(filter) == std::string::npos) continue;
        int32_t h[6];
        std::memcpy(h, fx + 8, 24);
        std::printf("fx %s: flags 0x%x totalSize %d msecLoopingLife %d elems looping %d oneShot %d emission %d\n", name, h[0], h[1], h[2],
                    h[3], h[4], h[5]);
        const uint8_t* elems;
        std::memcpy(&elems, fx + 32, 8);
        for (int i = 0; elems && i < h[3] + h[4] + h[5]; i++) {
            const uint8_t* e = elems + 288 * i;
            auto f = [&](int o) { float v; std::memcpy(&v, e + o, 4); return v; };
            auto n = [&](int o) { int32_t v; std::memcpy(&v, e + o, 4); return v; };
            std::printf("  elem %d %s flags 0x%08x spawn %d %d range %g %g fadeIn %g %g fadeOut %g %g cull %g delay %d %d life %d %d\n", i,
                        e[176] <= 12 ? kTypes[e[176]] : "?", static_cast<uint32_t>(n(0)), n(4), n(8), f(12), f(16), f(20), f(24), f(28),
                        f(32), f(36), n(40), n(44), n(48), n(52));
            std::printf("    org %g %g / %g %g / %g %g offRadius %g %g offHeight %g %g angles %g %g / %g %g / %g %g\n", f(56), f(60), f(64),
                        f(68), f(72), f(76), f(80), f(84), f(88), f(92), f(96), f(100), f(104), f(108), f(112), f(116));
            std::printf("    angVel %g %g / %g %g / %g %g initialRot %g %g gravity %g %g reflection %g %g\n", f(120), f(124), f(128), f(132),
                        f(136), f(140), f(144), f(148), f(152), f(156), f(160), f(164));
            std::printf("    atlas behavior %d index %d fps %d loops %d colBits %d rowBits %d entries %d | visuals %d velN %d visN %d\n", e[168],
                        e[169], e[170], e[171], e[172], e[173], e[174] | (e[175] << 8), e[177], e[178], e[179]);
            float cb[6];
            std::memcpy(cb, e + 208, 24);
            const char *imp, *death, *emit;
            std::memcpy(&imp, e + 232, 8);
            std::memcpy(&death, e + 240, 8);
            std::memcpy(&emit, e + 248, 8);
            std::printf("    collBounds %g %g %g / %g %g %g impact %s death %s emitted %s emitDist %g %g var %g %g\n", cb[0], cb[1], cb[2], cb[3],
                        cb[4], cb[5], imp ? imp : "-", death ? death : "-", emit ? emit : "-", f(256), f(260), f(264), f(268));
            const uint8_t* ext;
            std::memcpy(&ext, e + 272, 8);
            std::printf("    sortOrder %d lightingFrac %d useItemClip %d fadeInfo %d extended %s\n", e[280], e[281], e[282], e[283],
                        ext ? "yes" : "-");
            const uint8_t* vis;
            std::memcpy(&vis, e + 200, 8);
            auto visual = [&](const uint8_t* p) {
                if (!p) { std::printf("      (null)\n"); return; }
                const uint8_t t = e[176];
                if (t == 8 || t == 9) return;
                if (t == 10 || t == 12) { std::printf("      %s\n", reinterpret_cast<const char*>(p)); return; }
                if (t == 7) {
                    const char* mn;
                    std::memcpy(&mn, p, 8);
                    std::printf("      model %s\n", mn ? mn : "?");
                    return;
                }
                printMaterial(p);
            };
            if (e[176] == 11) {
                for (int k = 0; vis && k < 2 * e[177]; k++) {
                    const uint8_t* m;
                    std::memcpy(&m, vis + 8 * k, 8);
                    visual(m);
                }
            } else if (e[177] > 1) {
                for (int k = 0; vis && k < e[177]; k++) {
                    const uint8_t* p;
                    std::memcpy(&p, vis + 8 * k, 8);
                    visual(p);
                }
            } else if (e[177] == 1) {
                visual(vis);
            }
            if (samples) {
                const float* vs;
                std::memcpy(&vs, e + 184, 8);
                for (int k = 0; vs && k <= e[178]; k++) {
                    const float* s = vs + 24 * k;
                    std::printf("    vel[%d] local v %g %g %g +%g %g %g  d %g %g %g +%g %g %g | world v %g %g %g +%g %g %g  d %g %g %g +%g %g %g\n", k,
                                s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16],
                                s[17], s[18], s[19], s[20], s[21], s[22], s[23]);
                }
                const uint8_t* ss;
                std::memcpy(&ss, e + 192, 8);
                for (int k = 0; ss && k <= e[179]; k++) {
                    const uint8_t* s = ss + 48 * k;
                    float b[5], am[5];
                    std::memcpy(b, s + 4, 20);
                    std::memcpy(am, s + 28, 20);
                    std::printf("    vis[%d] color %u %u %u %u / %u %u %u %u rotDelta %g +%g rotTotal %g +%g size %g %g +%g %g scale %g +%g\n", k,
                                s[0], s[1], s[2], s[3], s[24], s[25], s[26], s[27], b[0], am[0], b[1], am[1], b[2], b[3], am[2], am[3], b[4],
                                am[4]);
                }
            }
        }
    }
    return 0;
}

// efxcheck <dir> [zoneCount]: parses + compiles every .efx under <dir> (e.g. BO1's raw/fx), then
// compiles the first zoneCount of them (default 300) into one SP zone and runs the loader on it.
int efxCheck(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path root = argv[0];
    const size_t zoneCount = argc >= 2 ? static_cast<size_t>(std::atoi(argv[1])) : 300;
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".efx") files.push_back(it->path());
    std::sort(files.begin(), files.end());
    size_t parsed = 0, compiled = 0, elems = 0;
    std::map<std::string, size_t> failures, warnKinds;
    std::map<int, size_t> types;
    auto lookup = [](const std::string&) { efx::MaterialInfo m; m.known = true; return m; };
    for (const fs::path& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        efx::Effect fx;
        std::string err;
        if (!efx::parse(text, fx, err)) {
            failures["parse: " + err]++;
            continue;
        }
        parsed++;
        efx::Compiled c;
        std::vector<std::string> warnings;
        if (!efx::compile(fx, f.stem().string(), lookup, c, warnings, err)) {
            failures["compile: " + err.substr(err.find(':') + 2)]++;
            continue;
        }
        compiled++;
        elems += c.elems.size();
        for (const efx::ElemOut& e : c.elems) types[e.type]++;
        for (const std::string& w : warnings) {
            std::string k = w.substr(w.find(':') + 2);
            if (k.find("material '") != std::string::npos) k = "material not in zone_source";
            else if (k.find("element '") == 0) k = "element " + k.substr(k.find("' ") + 2);
            else if (k.find("' ") != std::string::npos) k = k.substr(k.find("' ") + 2);
            warnKinds[k]++;
        }
    }
    static const char* kTypes[] = {"billboard", "oriented", "tail", "trail", "cloud", "sparkcloud", "sparkfountain",
                                   "model", "omnilight", "spotlight", "sound", "decal", "runner"};
    std::printf("%zu files: %zu parsed, %zu compiled, %zu elements\n", files.size(), parsed, compiled, elems);
    for (const auto& [t, n] : types) std::printf("  %-13s %zu\n", kTypes[t], n);
    for (const auto& [k, n] : failures) std::printf("  FAIL %zu x %s\n", n, k.c_str());
    for (const auto& [k, n] : warnKinds) std::printf("  warn %zu x %s\n", n, k.c_str());
    // zone + loader
    const fs::path tmp = fs::temp_directory_path() / "iw4r_efxcheck";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "zone_source" / "fx", ec);
    size_t copied = 0;
    for (const fs::path& f : files) {
        if (copied >= zoneCount) break;
        fs::path rel = fs::relative(f, root, ec);
        fs::create_directories((tmp / "zone_source" / "fx" / rel).parent_path(), ec);
        fs::copy_file(f, tmp / "zone_source" / "fx" / rel, fs::copy_options::overwrite_existing, ec);
        copied++;
    }
    Document d;
    d.addBrush(makeBox({-128, -128, -16}, {128, 128, 0}, "wc/me_concrete"));
    CompileOptions o;
    o.bakeLighting = false;
    o.zoneSourceDir = (tmp / "zone_source").string();
    const char* game = std::getenv("IW4R_GAME");
    o.gameDir = game ? game : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Call of Duty Modern Warfare 2";
    if (const char* la = std::getenv("LOCALAPPDATA")) o.cacheDir = std::string(la) + "\\iw4-radiant";
    std::vector<std::string> warnings;
    o.warnings = &warnings;
    std::vector<uint8_t> ff;
    std::string err;
    if (!compileMapToFastfile(d, "efxcheck", ff, err, MapTarget::SP, o)) {
        std::printf("zone compile failed: %s\n", err.c_str());
        return 1;
    }
    Fastfile zf;
    readFastfile(ff, zf, err);
    zone::SimulatedZone sim;
    const bool ok = zone::simulateLoad(zf.zone, sim, err);
    std::map<std::string, size_t> zw;
    for (const std::string& w : warnings) {
        if (w.find("built from Black Ops") != std::string::npos) std::printf("  %s\n", w.c_str());
        else if (w.rfind("fx material ", 0) == 0) zw[w.substr(w.find(": ") + 2, 40)]++;
    }
    for (const auto& [k, n] : zw) std::printf("  material warn %zu x %s\n", n, k.c_str());
    std::printf("zone: %zu effects copied, %zu fx assets, %zu materials, %zu bytes, loader %s %s\n", copied, sim.count(30, false),
                sim.count(5, false), zf.zone.size(),
                ok ? "ok" : "FAILED:", ok ? "" : err.c_str());
    fs::remove_all(tmp, ec);
    return ok ? 0 : 2;
}

int modelProbe(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Fastfile zf;
    std::string err;
    if (!readFastfile(bytes, zf, err)) { std::printf("%s\n", err.c_str()); return 1; }
    zone::SimulatedZone sim;
    size_t loaded = 0;
    std::string stopped;
    zone::simulatePrefix(zf.zone, sim, loaded, stopped);
    std::printf("loaded %zu assets; stopped: %s\n", loaded, stopped.c_str());
    int shown = 0;
    for (const auto& a : sim.assets) {
        if (a.type != 4 || a.reference || shown >= 6) continue;
        const iw4::XModel& m = *static_cast<const iw4::XModel*>(a.header);
        shown++;
        std::printf("xmodel %s: bones %u root %u surfs %u lodRamp %u scale %g numLods %u collLod %d flags 0x%x contents 0x%x radius %g collSurfs %d\n",
                    m.name, m.numBones, m.numRootBones, m.numsurfs, m.lodRampType, m.scale, m.numLods, (int)(signed char)m.collLod, m.flags,
                    m.contents, m.radius, m.numCollSurfs);
        std::printf("  bounds mid %g %g %g half %g %g %g  memUsage %d bad %d physPreset %s physCollmap %p\n",
                    m.bounds.midPoint[0], m.bounds.midPoint[1], m.bounds.midPoint[2], m.bounds.halfSize[0], m.bounds.halfSize[1],
                    m.bounds.halfSize[2], m.memUsage, m.bad, m.physPreset && m.physPreset->name ? m.physPreset->name : "-", (void*)m.physCollmap);
        for (int i = 0; i < m.numBones && i < 4; i++)
            std::printf("  bone %d name idx %u class %u baseMat q %g %g %g %g t %g %g %g w %g\n", i, m.boneNames ? m.boneNames[i] : 0,
                        m.partClassification ? m.partClassification[i] : 0, m.baseMat[i].quat[0], m.baseMat[i].quat[1], m.baseMat[i].quat[2],
                        m.baseMat[i].quat[3], m.baseMat[i].trans[0], m.baseMat[i].trans[1], m.baseMat[i].trans[2], m.baseMat[i].transWeight);
        if (std::getenv("IW4R_BONES")) {
            const int nr = m.numRootBones;
            if (std::getenv("IW4R_ALLBONES")) {  // name, parent, model-space bind pose, every bone
                for (int i = 0; i < m.numBones; i++) {
                    const int par = i < nr ? -1 : i - m.parentList[i - nr];
                    auto nm = [&](int b) { return m.boneNames[b] < sim.scriptStrings.size() ? sim.scriptStrings[m.boneNames[b]].c_str() : "?"; };
                    std::printf("  BONE %-3d %-32s parent %-28s base q %7.4f %7.4f %7.4f %7.4f t %8.3f %8.3f %8.3f\n", i, nm(i),
                                par < 0 ? "-" : nm(par), m.baseMat[i].quat[0], m.baseMat[i].quat[1], m.baseMat[i].quat[2],
                                m.baseMat[i].quat[3], m.baseMat[i].trans[0], m.baseMat[i].trans[1], m.baseMat[i].trans[2]);
                    if (i >= nr)
                        std::printf("           local q %6d %6d %6d %6d t %8.3f %8.3f %8.3f\n", m.quats[4 * (i - nr)], m.quats[4 * (i - nr) + 1],
                                    m.quats[4 * (i - nr) + 2], m.quats[4 * (i - nr) + 3], m.trans[3 * (i - nr)], m.trans[3 * (i - nr) + 1],
                                    m.trans[3 * (i - nr) + 2]);
                }
            }
            for (int i = 0; i < m.numBones && i < 14; i++) {
                std::printf("  B%-2d %-24s", i, m.boneNames && m.boneNames[i] < sim.scriptStrings.size() ? sim.scriptStrings[m.boneNames[i]].c_str() : "?");
                if (i >= nr) {
                    int j = i - nr;
                    std::printf(" parentByte %3u q %6d %6d %6d %6d t %8.3f %8.3f %8.3f", m.parentList[j], m.quats[4 * j], m.quats[4 * j + 1],
                                m.quats[4 * j + 2], m.quats[4 * j + 3], m.trans[3 * j], m.trans[3 * j + 1], m.trans[3 * j + 2]);
                }
                std::printf(" | base q %7.4f %7.4f %7.4f %7.4f t %8.3f %8.3f %8.3f w %g\n", m.baseMat[i].quat[0], m.baseMat[i].quat[1],
                            m.baseMat[i].quat[2], m.baseMat[i].quat[3], m.baseMat[i].trans[0], m.baseMat[i].trans[1], m.baseMat[i].trans[2],
                            m.baseMat[i].transWeight);
            }
            const iw4::XSurface& x = m.lodInfo[0].modelSurfs->surfs[0];
            std::printf("  surf0 vertCount %u blend counts %u %u %u %u\n  blend[0..40]:", x.vertCount, x.vertInfo.vertCount[0],
                        x.vertInfo.vertCount[1], x.vertInfo.vertCount[2], x.vertInfo.vertCount[3]);
            const uint16_t* vb = x.vertInfo.vertsBlend;
            if (!vb) { std::printf("\n"); continue; }
            for (int i = 0; i < 12; i++) std::printf(" %u", vb[i]);
            std::printf("\n  2-bone group start:");
            const uint16_t* g2 = vb + x.vertInfo.vertCount[0];
            for (int i = 0; i < 12; i++) std::printf(" %u", g2[i]);
            std::printf("\n  3-bone group start:");
            const uint16_t* g3 = g2 + 3 * x.vertInfo.vertCount[1];
            for (int i = 0; i < 15; i++) std::printf(" %u", g3[i]);
            std::printf("\n");
        }
        for (int l = 0; l < m.numLods; l++) {
            const iw4::XModelLodInfo& lod = m.lodInfo[l];
            std::printf("  lod %d dist %g numsurfs %u surfIndex %u smc %d/%d/%d/%d surfs %s\n", l, lod.dist, lod.numsurfs, lod.surfIndex,
                        lod.lod, lod.smcBaseIndexPlusOne, lod.smcSubIndexMask, lod.smcBucket,
                        lod.modelSurfs && lod.modelSurfs->name ? lod.modelSurfs->name : "-");
            if (!lod.modelSurfs || !lod.modelSurfs->surfs || l > 0) continue;
            for (int k = 0; k < lod.numsurfs && k < 3; k++) {
                const iw4::XSurface& x = lod.modelSurfs->surfs[k];
                std::printf("    surf %d tile %d deformed %d verts %u tris %u baseTri %u baseVert %u vertInfo %u/%u/%u/%u blend %p lists %u partBits %08x\n", k,
                            x.tileMode, x.deformed, x.vertCount, x.triCount, x.baseTriIndex, x.baseVertIndex, x.vertInfo.vertCount[0],
                            x.vertInfo.vertCount[1], x.vertInfo.vertCount[2], x.vertInfo.vertCount[3], (void*)x.vertInfo.vertsBlend,
                            x.vertListCount, x.partBits[0]);
                for (unsigned v = 0; v < x.vertListCount && v < 2; v++) {
                    const iw4::XRigidVertList& rl = x.vertList[v];
                    std::printf("      rigid bone %u verts %u triOff %u tris %u tree %p", rl.boneOffset, rl.vertCount, rl.triOffset, rl.triCount,
                                (void*)rl.collisionTree);
                    if (rl.collisionTree)
                        std::printf(" nodes %u leafs %u trans %g %g %g scale %g %g %g", rl.collisionTree->nodeCount, rl.collisionTree->leafCount,
                                    rl.collisionTree->trans[0], rl.collisionTree->trans[1], rl.collisionTree->trans[2],
                                    rl.collisionTree->scale[0], rl.collisionTree->scale[1], rl.collisionTree->scale[2]);
                    std::printf("\n");
                    if (rl.collisionTree && rl.collisionTree->nodeCount)
                        for (unsigned n = 0; n < rl.collisionTree->nodeCount && n < 3; n++) {
                            const auto& nd = rl.collisionTree->nodes[n];
                            std::printf("        node %u mins %u %u %u maxs %u %u %u child %u count %u\n", n, nd.aabb.mins[0], nd.aabb.mins[1],
                                        nd.aabb.mins[2], nd.aabb.maxs[0], nd.aabb.maxs[1], nd.aabb.maxs[2], nd.childBeginIndex, nd.childCount);
                        }
                }
                const iw4::GfxPackedVertex& pv = x.verts0[0];
                std::printf("      v0 xyz %g %g %g binSign %g color %08x tex %08x normal %08x tangent %08x\n", pv.xyz[0], pv.xyz[1], pv.xyz[2],
                            pv.binormalSign, pv.color.packed, pv.texCoord.packed, pv.normal.packed, pv.tangent.packed);
            }
        }
        for (int i = 0; i < m.numsurfs && m.materialHandles; i++)
            if (m.materialHandles[i]) {
                const iw4::Material* mt = m.materialHandles[i];
                std::printf("  material %d %s sortKey %u techset %s\n", i, mt->info.name ? mt->info.name : "?", mt->info.sortKey,
                            mt->techniqueSet && mt->techniqueSet->name ? mt->techniqueSet->name : "?");
            }
    }
    return 0;
}

// animprobe <zone.ff> : prefix-loads a stock zone and, for each XAnimParts reached, checks the
// key-data reading of the anim evaluator (iw4mp 0x2b4d30) against the declared array sizes.
int animProbe(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Fastfile zf;
    std::string err;
    if (!readFastfile(bytes, zf, err)) { std::printf("%s\n", err.c_str()); return 1; }
    zone::SimulatedZone sim;
    size_t loaded = 0;
    std::string stopped;
    zone::simulatePrefix(zf.zone, sim, loaded, stopped);
    std::printf("loaded %zu assets; stopped: %s\n", loaded, stopped.c_str());
    int shown = 0, ok = 0, total = 0;
    for (const auto& a : sim.assets) {
        if (a.type != 2) continue;
        const iw4::XAnimParts& x = *static_cast<const iw4::XAnimParts*>(a.header);
        total++;
        if (x.numframes >= 256) continue;
        const uint8_t* b = x.boneCount;
        size_t db = 0, ds = 0, di = 0, rds = 0, rdb = 0;
        auto sh = [&](size_t i) { return i < x.dataShortCount ? static_cast<uint16_t>(x.dataShort[i]) : 0; };
        for (int i = 0; i < b[1]; i++) { uint16_t n = sh(ds++); db += n + 1; rds += 2ull * (n + 1); }
        for (int i = 0; i < b[2]; i++) { uint16_t n = sh(ds++); db += n + 1; rds += 4ull * (n + 1); }
        ds += 2ull * b[3] + 4ull * b[4];
        for (int i = 0; i < b[5]; i++) { uint16_t n = sh(ds++); db += 1 + n + 1; di += 6; rdb += 3ull * (n + 1); }
        for (int i = 0; i < b[6]; i++) { uint16_t n = sh(ds++); db += 1 + n + 1; di += 6; rds += 3ull * (n + 1); }
        db += b[7] + b[8];
        di += 3ull * b[7];
        if (std::getenv("IW4R_FRAMES") && x.boneCount[2]) {  // first full-quat bone's key frames + first key
            size_t s0 = x.boneCount[1] ? 0 : 0, d0 = 0;
            for (int i = 0; i < b[1]; i++) { uint16_t n = sh(s0++); d0 += n + 1; }
            uint16_t n = sh(s0);
            std::printf("  %s: full-quat bone 0: %u keys, frames:", x.name, n + 1);
            for (unsigned k = 0; k <= n && k < 40; k++) std::printf(" %u", static_cast<uint8_t>(x.dataByte[d0 + k]));
            std::printf("\n");
        }
        const bool match = db == x.dataByteCount && ds == x.dataShortCount && di == x.dataIntCount &&
                           rds == x.randomDataShortCount && rdb == x.randomDataByteCount;
        ok += match;
        if (shown++ < 8 || !match)
            std::printf("%-40s frames %3u rate %4.1f flags 0x%02x bones %u [%u %u %u %u %u | %u %u %u %u] idx %u delta %s | "
                        "byte %zu/%u short %zu/%u int %zu/%u rshort %zu/%u rbyte %zu/%u rint %u %s\n",
                        x.name, x.numframes, x.framerate, (unsigned)(uint8_t)x.flags, b[9], b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                        b[8], x.indexCount, x.deltaPart ? "yes" : "no", db, x.dataByteCount, ds, x.dataShortCount, di, x.dataIntCount, rds,
                        x.randomDataShortCount, rdb, x.randomDataByteCount, x.randomDataIntCount, match ? "OK" : "MISMATCH");
    }
    std::printf("%d of %d short anims match the decoding (%d anims total)\n", ok, shown, total);
    return 0;
}

int liveWorld() {
    std::string report, err;
    if (!live::inspectWorld(report, err, gLiveGame)) {
        std::printf("live world failed: %s\n", err.c_str());
        return 1;
    }
    std::fputs(report.c_str(), stdout);
    return 0;
}

int liveWeapon(const char* name) {
    std::string report, err;
    if (!live::dumpWeapons(name ? name : "", report, err, gLiveGame)) {
        std::printf("liveweapon failed: %s\n", err.c_str());
        return 1;
    }
    std::fputs(report.c_str(), stdout);
    return 0;
}

int liveProbe() {
    std::string report, err;
    if (!live::probeProcess(report, err, gLiveGame)) {
        std::printf("live probe failed: %s\n", err.c_str());
        return 1;
    }
    std::fputs(report.c_str(), stdout);
    return 0;
}

int dump(const std::string& in, const std::string& outRoot) {
    zone::DumpResult r;
    std::string err;
    if (!zone::dumpFastfile(in, outRoot, r, err)) {
        std::printf("dump failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("dumped %s -> %s\n  %s\n", in.c_str(), r.folder.c_str(), r.summary.c_str());
    for (const std::string& f : r.files) std::printf("  %s\n", f.c_str());
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    // Strip --mp / --sp (pick the game for the live* commands) wherever they appear.
    int n = 1;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--mp") gLiveGame = "iw4mp.exe";
        else if (a == "--sp") gLiveGame = "iw4sp.exe";
        else if (a == "--cpubake") gCpuBake = true;
        else argv[n++] = argv[i];
    }
    argc = n;
    if (argc >= 3 && std::string(argv[1]) == "info") return info(argv[2]);
    if (argc >= 4 && std::string(argv[1]) == "rebuild") return rebuild(argv[2], argv[3]);
    if (argc >= 4 && std::string(argv[1]) == "compile") return compile(argv[2], argv[3]);
    if (argc >= 4 && std::string(argv[1]) == "lightviz") return lightViz(argv[2], argv[3], argc >= 5 ? argv[4] : nullptr, argc >= 6 ? argv[5] : nullptr);
    if (argc >= 3 && std::string(argv[1]) == "bakebench") return bakeBench(argv[2], argc >= 4 ? argv[3] : nullptr);
    if (argc >= 3 && std::string(argv[1]) == "verify") return verify(argv[2]);
    if (argc >= 3 && std::string(argv[1]) == "dump") return dump(argv[2], argc >= 4 ? argv[3] : ".");
    if (argc >= 3 && std::string(argv[1]) == "import") return importMap(argv[2], argc >= 4 ? argv[3] : "");
    if (argc >= 2 && std::string(argv[1]) == "livedump") return liveDump(argc >= 3 ? argv[2] : "");
    if (argc >= 2 && std::string(argv[1]) == "liveprobe") return liveProbe();
    if (argc >= 2 && std::string(argv[1]) == "liveweapon") return liveWeapon(argc >= 3 ? argv[2] : nullptr);
    if (argc >= 2 && std::string(argv[1]) == "liveworld") return liveWorld();
    if (argc >= 3 && std::string(argv[1]) == "matdefs") return matDefs(argc - 2, argv + 2);
    if (argc >= 4 && std::string(argv[1]) == "refprobe") return refProbe(argc - 2, argv + 2);
    if (argc >= 3 && std::string(argv[1]) == "modelprobe") return modelProbe(argv[2]);
    if (argc >= 3 && std::string(argv[1]) == "fxprobe") return fxProbe(argc - 2, argv + 2);
    if (argc >= 3 && std::string(argv[1]) == "efxcheck") return efxCheck(argc - 2, argv + 2);
    if (argc >= 3 && std::string(argv[1]) == "animprobe") return animProbe(argv[2]);
    if (argc >= 3 && std::string(argv[1]) == "neutrallut") {  // a strip LUT to grade in an image editor
        std::string err;
        const int size = argc >= 4 ? std::atoi(argv[3]) : 32;
        if (!light::saveNeutralStripLut(argv[2], size, err)) {
            std::printf("error: %s\n", err.c_str());
            return 1;
        }
        std::printf("wrote %s (neutral %dx%dx%d LUT strip)\n", argv[2], size, size, size);
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "livemodels") return liveModels(argc - 2, argv + 2);
    std::printf("usage:\n"
                "  iw4r_zone info <in.ff>\n"
                "  iw4r_zone rebuild <in.ff> <out.ff>\n"
                "  iw4r_zone compile <map.map> <out.ff>\n"
                "  iw4r_zone verify <in.ff>\n"
                "  iw4r_zone dump <in.ff> [out-dir]\n"
                "  iw4r_zone import <map.ff> [out.map]\n"
                "  iw4r_zone livedump [out.map]   (reads the running iw4mp.exe or iw4sp.exe; load a map first)\n"
                "  iw4r_zone liveprobe | livemodels <name...>\n"
                "  iw4r_zone neutrallut <out.png> [size]   (identity colour-grade LUT strip, default 32)\n"
                "  --mp / --sp pick the game (compile default MP; live* default: whichever is running)\n");
    return 1;
}
