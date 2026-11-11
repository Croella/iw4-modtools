#include "zonedump.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <miniz.h>
#include "fastfile.h"
#include "iw4.h"

namespace fs = std::filesystem;

namespace zone {
namespace {

std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

bool writeBytes(const fs::path& p, const void* data, size_t n) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
    return static_cast<bool>(f);
}

// A zone-internal name as a safe relative path: no drive, no root, no "..".
fs::path safeRelative(const std::string& name) {
    fs::path out;
    for (const fs::path& part : fs::path(name).relative_path()) {
        std::string s = part.string();
        if (s.empty() || s == "." || s == "..") continue;
        for (char& c : s)
            if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        out /= s;
    }
    return out.empty() ? fs::path("unnamed") : out;
}

struct AssetEntry {
    uint32_t type;
    uint64_t header;
};

// The XAssetList's asset array: after the script-string pointers and the inline strings.
bool readAssetList(const std::vector<uint8_t>& z, std::vector<std::string>& strings, std::vector<AssetEntry>& assets) {
    if (z.size() < 72) return false;
    uint32_t sc, ac;
    uint64_t sp, ap;
    std::memcpy(&sc, z.data() + 40, 4);
    std::memcpy(&sp, z.data() + 48, 8);
    std::memcpy(&ac, z.data() + 56, 4);
    std::memcpy(&ap, z.data() + 64, 8);
    size_t o = 72;
    if (sp) {
        if (o + 8ull * sc > z.size()) return false;
        std::vector<uint64_t> ptrs(sc);
        std::memcpy(ptrs.data(), z.data() + o, 8ull * sc);
        o += 8ull * sc;
        for (uint64_t p : ptrs) {
            if (p != ~0ull) continue;
            const void* end = std::memchr(z.data() + o, 0, z.size() - o);
            if (!end) return false;
            size_t n = static_cast<const uint8_t*>(end) - (z.data() + o);
            strings.emplace_back(reinterpret_cast<const char*>(z.data() + o), n);
            o += n + 1;
        }
    }
    if (ap) {
        if (o + 16ull * ac > z.size()) return false;
        for (uint32_t i = 0; i < ac; i++) {
            AssetEntry e;
            std::memcpy(&e.type, z.data() + o + 16ull * i, 4);
            std::memcpy(&e.header, z.data() + o + 16ull * i + 8, 8);
            assets.push_back(e);
        }
    }
    return true;
}

const char* headerKind(uint64_t h) { return h == ~0ull ? "inline" : h == ~1ull ? "inline+slot" : "reference"; }

}  // namespace

const char* assetTypeName(uint32_t t) {
    static const char* names[] = {
        "physpreset", "phys_collmap", "xanimparts", "xmodel_surfs", "xmodel", "material", "pixelshader",
        "vertexshader", "vertexdecl", "techniqueset", "image", "sound", "sound_curve", "loaded_sound",
        "clipmap_sp", "clipmap_mp", "comworld", "game_map_sp", "game_map_mp", "map_ents", "fx_map", "gfx_map",
        "lightdef", "ui_map", "font", "menufile", "menu", "localize", "weapon", "snddriverglobals", "fx",
        "impactfx", "aitype", "mptype", "character", "xmodelalias", "rawfile", "stringtable", "leaderboard",
        "structureddatadef", "tracer", "vehicle", "addon_map_ents"};
    return t < sizeof(names) / sizeof(names[0]) ? names[t] : "unknown";
}

std::string describeSimulatedZone(const SimulatedZone& sim) {
    static const char* blocks[8] = {"TEMP", "PHYSICAL", "RUNTIME", "VIRTUAL", "LARGE", "CALLBACK", "VERTEX", "INDEX"};
    std::string s;
    for (int b = 0; b < 8; b++)
        if (sim.blockSize[b]) s += fmt("  block %-8s size %10u  used %10u\n", blocks[b], sim.blockSize[b], sim.blockUsed[b]);
    std::map<std::string, std::pair<size_t, size_t>> counts;  // type -> {assets, references}
    for (const LoadedAsset& a : sim.assets) {
        auto& c = counts[assetTypeName(a.type)];
        (a.reference ? c.second : c.first)++;
    }
    for (auto& [name, c] : counts)
        s += fmt("  %-14s %zu%s\n", name.c_str(), c.first, c.second ? fmt(" (+%zu by-name references)", c.second).c_str() : "");
    if (auto* c = static_cast<const iw4::clipMap_t*>(sim.find(15)))
        s += fmt("  clipMap '%s': %u planes, %u brushes, %u sides, %u materials, %u leafs, %u nodes\n", c->name,
                 c->planeCount, c->numBrushes, c->numBrushSides, c->numMaterials, c->numLeafs, c->numNodes);
    if (auto* g = static_cast<const iw4::GfxWorld*>(sim.find(21)))
        s += fmt("  GfxWorld '%s': %u surfaces, %u vertices, %u indices, %d materials, %u light grid entries\n",
                 g->name, g->surfaceCount, g->draw.vertexCount, g->draw.indexCount, g->materialMemoryCount,
                 g->lightGrid.entryCount);
    if (auto* m = static_cast<const iw4::MapEnts*>(sim.find(19)))
        s += fmt("  MapEnts '%s': %d entity chars\n", m->name, m->numEntityChars);
    return s;
}

bool dumpFastfile(const std::string& ffPath, const std::string& outRoot, DumpResult& out, std::string& error) {
    out = DumpResult{};
    std::vector<uint8_t> file;
    {
        std::ifstream f(ffPath, std::ios::binary | std::ios::ate);
        if (!f) {
            error = "can't read " + ffPath;
            return false;
        }
        file.resize(static_cast<size_t>(f.tellg()));
        f.seekg(0);
        if (!file.empty() && !f.read(reinterpret_cast<char*>(file.data()), file.size())) {
            error = "can't read " + ffPath;
            return false;
        }
    }
    const std::string stem = fs::path(ffPath).stem().string();
    const fs::path folder = (fs::path(outRoot) / safeRelative(stem)).make_preferred();
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        error = "can't create " + folder.string() + ": " + ec.message();
        return false;
    }
    out.folder = folder.string();

    std::string report = fmt("fastfile: %s\nsize:     %zu bytes\n", ffPath.c_str(), file.size());
    auto finish = [&](bool ok) {
        if (!writeBytes(folder / "report.txt", report.data(), report.size())) {
            error = "can't write report.txt";
            return false;
        }
        out.files.insert(out.files.begin(), "report.txt");
        return ok;
    };

    FastfileKind kind = classifyFastfile(file.data(), file.size());
    if (kind != FastfileKind::Unsigned) {
        report += kind == FastfileKind::Signed
                      ? "kind:     signed (IWffs100) - not readable: the payload is not a plain zlib stream\n"
                      : "kind:     not an IW fastfile\n";
        out.summary = kind == FastfileKind::Signed ? "signed fastfile: header only (payload not readable)"
                                                   : "not an IW fastfile";
        return finish(true);
    }
    Fastfile ff;
    if (!readFastfile(file, ff, error)) {
        report += "read failed: " + error + "\n";
        finish(false);
        return false;
    }
    const std::vector<uint8_t>& z = ff.zone;
    report += "kind:     unsigned (IWffu100), version 276\n";
    report += "prefix:   ";
    for (uint8_t b : ff.prefix) report += fmt("%02x ", b);
    report += fmt("\ninflated: %zu bytes -> %s.zone\n", z.size(), stem.c_str());
    if (!writeBytes(folder / (stem + ".zone"), z.data(), z.size())) {
        error = "can't write the inflated zone";
        return false;
    }
    out.files.push_back(stem + ".zone");

    if (z.size() >= 40) {
        iw4::XFile xf;
        std::memcpy(&xf, z.data(), sizeof(xf));
        report += fmt("XFile:    size %u, externalSize %u\nblocks:  ", xf.size, xf.externalSize);
        for (unsigned b : xf.blockSize) report += fmt(" %u", b);
        report += "\n";
    }
    std::vector<std::string> strings;
    std::vector<AssetEntry> assets;
    if (!readAssetList(z, strings, assets)) {
        report += "asset list: unreadable\n";
        out.summary = "inflated, but the asset list is unreadable";
        return finish(true);
    }
    report += fmt("\nscript strings: %zu\n", strings.size());
    for (size_t i = 0; i < strings.size() && i < 64; i++) report += fmt("  %zu  %s\n", i, strings[i].c_str());
    if (strings.size() > 64) report += fmt("  ... %zu more\n", strings.size() - 64);
    std::map<uint32_t, size_t> typeCounts;
    for (const AssetEntry& a : assets) typeCounts[a.type]++;
    report += fmt("\nassets: %zu\n", assets.size());
    for (auto& [t, n] : typeCounts) report += fmt("  %-18s (%2u) x %zu\n", assetTypeName(t), t, n);
    report += "\nasset list (load order):\n";
    for (size_t i = 0; i < assets.size(); i++)
        report += fmt("  %5zu  %-18s %s\n", i, assetTypeName(assets[i].type), headerKind(assets[i].header));

    // Walk it like the game; on success extract what we can read as files.
    SimulatedZone sim;
    std::string simErr;
    out.simulated = simulateLoad(z, sim, simErr);
    report += "\nloader simulation: ";
    if (!out.simulated) {
        report += "stopped: " + simErr + "\n  (the simulator covers the asset types iw4-radiant writes; stock zones with\n"
                  "   other types are listed above but their contents aren't extracted)\n";
        out.summary = fmt("%zu assets listed; contents not extracted (%s)", assets.size(), simErr.c_str());
        return finish(true);
    }
    report += "ok\n" + describeSimulatedZone(sim);
    size_t rawfiles = 0;
    report += "\nextracted:\n";
    for (const LoadedAsset& a : sim.assets) {
        if (a.type == 36) {
            auto* r = static_cast<const iw4::RawFile*>(a.header);
            if (!r->name || !r->buffer) continue;
            std::vector<uint8_t> data;
            if (r->compressedLen > 0) {
                data.resize(static_cast<size_t>(r->len));
                mz_ulong n = static_cast<mz_ulong>(data.size());
                if (mz_uncompress(data.data(), &n, reinterpret_cast<const unsigned char*>(r->buffer),
                                  static_cast<mz_ulong>(r->compressedLen)) != MZ_OK) {
                    report += fmt("  %s: decompression failed\n", r->name);
                    continue;
                }
                data.resize(n);
            } else {
                data.assign(r->buffer, r->buffer + r->len);
            }
            fs::path rel = fs::path("files") / safeRelative(r->name);
            if (writeBytes(folder / rel, data.data(), data.size())) {
                out.files.push_back(rel.generic_string());
                report += fmt("  %s (%zu bytes)\n", rel.generic_string().c_str(), data.size());
                rawfiles++;
            }
        } else if (a.type == 19) {
            auto* m = static_cast<const iw4::MapEnts*>(a.header);
            if (m->entityString && m->numEntityChars > 0 &&
                writeBytes(folder / "entities.txt", m->entityString, static_cast<size_t>(m->numEntityChars - 1))) {
                out.files.push_back("entities.txt");
                report += fmt("  entities.txt (%d chars)\n", m->numEntityChars - 1);
            }
        }
    }
    out.summary = fmt("%zu assets, loader simulation ok, %zu rawfiles extracted", sim.assets.size(), rawfiles);
    return finish(true);
}

}  // namespace zone
