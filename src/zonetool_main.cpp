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
#include <chrono>
#include "zone/mapimport.h"
#include "live/liveimport.h"
#include "zone/zonedump.h"
#include "zone/zonesim.h"
#include "zone/matdefs.h"

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
    std::vector<std::string> warnings;
    options.warnings = &warnings;
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
    if (!writeFile(outPath, ff)) {
        std::printf("error: can't write %s\n", outPath.c_str());
        return 1;
    }
    std::printf("  wrote %s (%zu bytes): ComWorld, FxWorld, GfxWorld, %s, clipMap (+MapEnts, %zu entities)\n",
                outPath.c_str(), ff.size(), target == MapTarget::SP ? "GameWorldSp" : "GameWorldMp",
                doc.ents.size());
    std::printf("  not yet in the zone: non-world/unknown materials (by name), spot lights, brush-entity\n"
                "  submodels, patches, static models\n");
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

int liveWorld() {
    std::string report, err;
    if (!live::inspectWorld(report, err, gLiveGame)) {
        std::printf("live world failed: %s\n", err.c_str());
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
        else argv[n++] = argv[i];
    }
    argc = n;
    if (argc >= 3 && std::string(argv[1]) == "info") return info(argv[2]);
    if (argc >= 4 && std::string(argv[1]) == "rebuild") return rebuild(argv[2], argv[3]);
    if (argc >= 4 && std::string(argv[1]) == "compile") return compile(argv[2], argv[3]);
    if (argc >= 3 && std::string(argv[1]) == "verify") return verify(argv[2]);
    if (argc >= 3 && std::string(argv[1]) == "dump") return dump(argv[2], argc >= 4 ? argv[3] : ".");
    if (argc >= 3 && std::string(argv[1]) == "import") return importMap(argv[2], argc >= 4 ? argv[3] : "");
    if (argc >= 2 && std::string(argv[1]) == "livedump") return liveDump(argc >= 3 ? argv[2] : "");
    if (argc >= 2 && std::string(argv[1]) == "liveprobe") return liveProbe();
    if (argc >= 2 && std::string(argv[1]) == "liveworld") return liveWorld();
    if (argc >= 3 && std::string(argv[1]) == "matdefs") return matDefs(argc - 2, argv + 2);
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
                "  --mp / --sp pick the game (compile default MP; live* default: whichever is running)\n");
    return 1;
}
