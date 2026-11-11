#pragma once
#include <atomic>
#include "zone/matdefs.h"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "iwi.h"

struct MaterialEntry {
    std::string name;    // as used in a .map, without the "wc/" style prefix
    std::string prefix;  // "wc", "mc", ... or empty
    std::string image;   // colorMap image name; empty if it couldn't be resolved
};

// Indexes IWD archives for textures and scans fastfiles for materials on a background
// thread, caching results so later launches start immediately.
class AssetDb {
public:
    using LogFn = std::function<void(const std::string&)>;
    AssetDb();
    ~AssetDb();

    void init(const std::string& gamePath, const std::string& cacheDir, LogFn log);
    void shutdown();

    bool hasGame() const { return gameFound; }
    size_t imageCount() const { return imageIndex.size(); }

    // Only changes when the material list does; poll this to refresh a cached copy.
    uint64_t materialsVersion() const { return version.load(); }
    std::vector<MaterialEntry> materials();

    int zonesDone() const { return scanDone.load(); }
    int zonesTotal() const { return scanTotal.load(); }
    bool scanning() const { return scanRunning.load(); }

    // Material -> colorMap image pairs learned elsewhere (e.g. a map import's own materials);
    // fills in names the zone scan couldn't resolve. Full names ("wc/x") or bare names.
    void addMaterialHints(const std::map<std::string, std::string>& materialToImage);

    // Stock world-material definitions found by the zone scan, for embedding in compiled maps
    // (cached in materialdefs.bin; the compiler reads that file).
    size_t materialDefCount();

    // Main thread only: zip readers aren't thread safe.
    bool loadMaterialImage(const std::string& materialName, int maxDim, IwiImage& out);

private:
    struct Zip;
    struct ImageRef {
        int zip;
        unsigned file;
    };
    void indexIwds(const std::string& mainDir);
    void loadCache();
    void appendCache(const std::string& zone, uint64_t size, uint64_t mtime,
                     const std::map<std::string, std::string>& found);
    void scanThread(std::vector<std::string> zones);
    void addMaterial(const std::string& fullName, const std::string& image);
    bool readImage(const std::string& imageName, int maxDim, IwiImage& out);

    LogFn log;
    bool gameFound = false;
    std::string cachePath, defsPath;
    matdef::Library defs;  // guarded by mutex
    bool defsLoaded = false;
    std::vector<std::unique_ptr<Zip>> zips;
    std::unordered_map<std::string, ImageRef> imageIndex;

    std::mutex mutex;  // guards materialMap and scanned
    std::map<std::string, MaterialEntry> materialMap;
    std::map<std::string, std::pair<uint64_t, uint64_t>> scanned;

    std::thread worker;
    std::atomic<bool> stopping{false}, scanRunning{false};
    std::atomic<int> scanDone{0}, scanTotal{0};
    std::atomic<uint64_t> version{1};
};
