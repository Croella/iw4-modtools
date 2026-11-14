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
#include "light/lightbake.h"
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
    const std::string* pack = doc.ents.empty() ? nullptr : doc.ents[0].find("weaponpack");
    if (pack && !pack->empty() && target == MapTarget::SP) options.addonLevel = weaponPackLevel(mapName);
    if (readMapScript(mapPath, options.mapScript))
        std::printf("map script: %s\n", mapScriptPath(mapPath).c_str());
    options.zoneSourceDir = zoneSourcePath(mapPath);
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
    if (argc >= 2 && std::string(argv[1]) == "liveweapon") return liveWeapon(argc >= 3 ? argv[2] : nullptr);
    if (argc >= 2 && std::string(argv[1]) == "liveworld") return liveWorld();
    if (argc >= 3 && std::string(argv[1]) == "matdefs") return matDefs(argc - 2, argv + 2);
    if (argc >= 4 && std::string(argv[1]) == "refprobe") return refProbe(argc - 2, argv + 2);
    if (argc >= 3 && std::string(argv[1]) == "modelprobe") return modelProbe(argv[2]);
    if (argc >= 3 && std::string(argv[1]) == "animprobe") return animProbe(argv[2]);
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
