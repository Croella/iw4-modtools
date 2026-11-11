#include "assets.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <miniz.h>
#include "fastfile.h"

namespace fs = std::filesystem;

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool readFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    out.resize(static_cast<size_t>(f.tellg()));
    f.seekg(0);
    return out.empty() || static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), out.size()));
}

uint64_t fileStamp(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    return ec ? 0 : static_cast<uint64_t>(t.time_since_epoch().count());
}
}  // namespace

struct AssetDb::Zip {
    mz_zip_archive archive{};
    ~Zip() { mz_zip_reader_end(&archive); }
};

AssetDb::AssetDb() = default;
AssetDb::~AssetDb() { shutdown(); }

void AssetDb::shutdown() {
    stopping = true;
    if (worker.joinable()) worker.join();
    zips.clear();
    imageIndex.clear();
}

void AssetDb::init(const std::string& gamePath, const std::string& cacheDir, LogFn logFn) {
    shutdown();
    log = std::move(logFn);
    stopping = false;
    {
        std::lock_guard lock(mutex);
        materialMap.clear();
        scanned.clear();
    }
    fs::path game{gamePath};
    std::error_code ec;
    gameFound = fs::is_directory(game / "main", ec) && fs::is_directory(game / "zone", ec);
    if (!gameFound) {
        log("Game not found at " + gamePath + ". Set the path in Edit > Settings.");
        version++;
        return;
    }
    indexIwds((game / "main").string());
    log("Indexed " + std::to_string(imageIndex.size()) + " images from " + std::to_string(zips.size()) + " IWDs");

    fs::create_directories(fs::path(cacheDir), ec);
    cachePath = (fs::path(cacheDir) / "materials.cache").string();
    loadCache();
    defsPath = (fs::path(cacheDir) / "materialdefs.bin").string();
    {
        std::lock_guard lock(mutex);
        defs.clear();
        defsLoaded = matdef::load(defsPath, defs);
    }
    if (defsLoaded)
        log("Loaded " + std::to_string(defs.size()) + " material definitions from cache");
    else
        log("Building the material library (one-time zone rescan) so compiled maps can embed their materials ...");

    std::vector<std::string> zones;
    for (const auto& entry : fs::directory_iterator(game / "zone" / "english", ec)) {
        if (entry.path().extension() != ".ff") continue;
        std::string stem = entry.path().stem().string();
        if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, "_load") == 0) continue;
        zones.push_back(entry.path().string());
    }
    std::sort(zones.begin(), zones.end());
    scanTotal = static_cast<int>(zones.size());
    scanDone = 0;
    scanRunning = true;
    version++;
    worker = std::thread(&AssetDb::scanThread, this, std::move(zones));
}

void AssetDb::indexIwds(const std::string& mainDir) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(mainDir, ec))
        if (entry.path().extension() == ".iwd") files.push_back(entry.path());
    std::sort(files.begin(), files.end());  // later archives override earlier ones

    for (const fs::path& p : files) {
        auto zip = std::make_unique<Zip>();
        if (!mz_zip_reader_init_file(&zip->archive, p.string().c_str(), 0)) {
            log("Couldn't open " + p.filename().string());
            continue;
        }
        int zipIndex = static_cast<int>(zips.size());
        mz_uint count = mz_zip_reader_get_num_files(&zip->archive);
        for (mz_uint i = 0; i < count; i++) {
            char name[260];
            mz_zip_reader_get_filename(&zip->archive, i, name, sizeof name);
            std::string n = lower(name);
            if (n.size() > 11 && n.rfind("images/", 0) == 0 && n.compare(n.size() - 4, 4, ".iwi") == 0)
                imageIndex[n.substr(7, n.size() - 11)] = {zipIndex, i};
        }
        zips.push_back(std::move(zip));
    }
}

void AssetDb::addMaterialHints(const std::map<std::string, std::string>& materialToImage) {
    {
        std::lock_guard lock(mutex);
        for (auto& [material, image] : materialToImage) addMaterial(material, image);
    }
    version++;
}

void AssetDb::addMaterial(const std::string& fullName, const std::string& image) {
    MaterialEntry e;
    size_t slash = fullName.find('/');
    if (slash != std::string::npos) {
        e.prefix = fullName.substr(0, slash);
        e.name = fullName.substr(slash + 1);
    } else {
        e.name = fullName;
    }
    e.image = lower(image);
    // World materials win: they're the ones a map actually uses.
    auto it = materialMap.find(e.name);
    if (it == materialMap.end()) {
        materialMap.emplace(e.name, std::move(e));
    } else {
        bool better = e.prefix == "wc" && it->second.prefix != "wc";
        if (better) {
            if (e.image.empty()) e.image = it->second.image;
            it->second = std::move(e);
        } else if (it->second.image.empty()) {
            it->second.image = e.image;
        }
    }
}

void AssetDb::loadCache() {
    std::ifstream f{fs::path{cachePath}};
    if (!f) return;
    std::string line;
    std::lock_guard lock(mutex);
    while (std::getline(f, line)) {
        std::istringstream in(line);
        std::string tag, a, b, c;
        std::getline(in, tag, '\t');
        std::getline(in, a, '\t');
        std::getline(in, b, '\t');
        std::getline(in, c, '\t');
        if (tag == "Z" && !a.empty()) scanned[a] = {std::strtoull(b.c_str(), nullptr, 10), std::strtoull(c.c_str(), nullptr, 10)};
        else if (tag == "M" && !a.empty()) addMaterial(a, b);
    }
    log("Loaded " + std::to_string(materialMap.size()) + " materials from cache");
}

void AssetDb::appendCache(const std::string& zone, uint64_t size, uint64_t mtime,
                          const std::map<std::string, std::string>& found) {
    std::ofstream f{fs::path{cachePath}, std::ios::app};
    for (const auto& [name, image] : found) f << "M\t" << name << '\t' << image << '\n';
    f << "Z\t" << zone << '\t' << size << '\t' << mtime << '\n';  // written last so a partial scan reruns
}

void AssetDb::scanThread(std::vector<std::string> zones) {
    int newZones = 0;
    for (const std::string& path : zones) {
        if (stopping) break;
        fs::path p(path);
        std::string zone = p.filename().string();
        std::error_code ec;
        uint64_t size = fs::file_size(p, ec), stamp = fileStamp(p);
        {
            std::lock_guard lock(mutex);
            auto it = scanned.find(zone);
            if (defsLoaded && it != scanned.end() && it->second == std::make_pair(size, stamp)) {
                scanDone++;
                continue;
            }
        }
        std::vector<uint8_t> file, data;
        std::string err;
        std::map<std::string, std::string> found;
        if (!readFile(p, file)) {
            log("Couldn't read " + zone);
        } else if (!inflateFastfile(file, data, err)) {
            log(zone + ": " + err);
        } else {
            scanMaterials(data, found);
            matdef::Library zoneDefs;
            matdef::scan(data, zoneDefs);
            std::lock_guard lock(mutex);
            for (auto& [k, d] : zoneDefs)
                if (!defs.count(k) || d.textures.size() >= defs[k].textures.size()) defs[k] = std::move(d);
        }
        if (stopping) break;
        {
            std::lock_guard lock(mutex);
            for (const auto& [name, image] : found) addMaterial(name, image);
            scanned[zone] = {size, stamp};
        }
        appendCache(zone, size, stamp, found);
        newZones++;
        scanDone++;
        version++;
    }
    if (!stopping && newZones) {
        std::lock_guard lock(mutex);
        matdef::save(defsPath, defs);
        defsLoaded = true;
    }
    scanRunning = false;
    version++;
    if (!stopping) {
        if (newZones) log("Scanned " + std::to_string(newZones) + " new zones");
    }
}

size_t AssetDb::materialDefCount() {
    std::lock_guard lock(mutex);
    return defs.size();
}

std::vector<MaterialEntry> AssetDb::materials() {
    std::lock_guard lock(mutex);
    std::vector<MaterialEntry> out;
    out.reserve(materialMap.size());
    for (const auto& [name, entry] : materialMap) out.push_back(entry);
    return out;
}

bool AssetDb::readImage(const std::string& imageName, int maxDim, IwiImage& out) {
    auto it = imageIndex.find(imageName);
    if (it == imageIndex.end()) return false;
    size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(&zips[it->second.zip]->archive, it->second.file, &size, 0);
    if (!data) return false;
    std::string err;
    bool ok = parseIwi(static_cast<uint8_t*>(data), size, maxDim, out, err);
    mz_free(data);
    return ok;
}

bool AssetDb::loadMaterialImage(const std::string& materialName, int maxDim, IwiImage& out) {
    std::string image;
    {
        std::lock_guard lock(mutex);
        auto it = materialMap.find(materialName);
        if (it != materialMap.end()) image = it->second.image;
    }
    if (!image.empty() && readImage(image, maxDim, out)) return true;
    // Fallback for back-referenced images: the usual naming conventions.
    std::string base = lower(materialName);
    for (const char* suffix : {"", "_col", "_c", "_color"})
        if (readImage(base + suffix, maxDim, out)) return true;
    return false;
}
