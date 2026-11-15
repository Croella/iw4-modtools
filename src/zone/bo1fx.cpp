#include "zone/bo1fx.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <miniz.h>

#include "iwi.h"

namespace bo1 {
namespace {
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool readAll(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}
}  // namespace

std::string defaultRoot(const std::string& mw2GameDir) {
    if (const char* env = std::getenv("IW4R_BO1")) return env;
    if (mw2GameDir.empty()) return {};
    return (std::filesystem::path(mw2GameDir).parent_path() / "Call of Duty Black Ops").string();
}

Install::Install(std::string root) : root_(std::move(root)) {
    std::error_code ec;
    valid_ = !root_.empty() && std::filesystem::is_directory(std::filesystem::path(root_) / "raw" / "materials", ec);
}

bool Install::material(const std::string& name, EffectMaterial& out) const {
    if (!valid_) return false;
    std::vector<uint8_t> d;
    if (!readAll(std::filesystem::path(root_) / "raw" / "materials" / name, d) || d.size() < 64) return false;
    auto u16 = [&](size_t o) { uint16_t v = 0; if (o + 2 <= d.size()) std::memcpy(&v, d.data() + o, 2); return v; };
    auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= d.size()) std::memcpy(&v, d.data() + o, 4); return v; };
    auto str = [&](uint32_t o) {
        std::string s;
        for (size_t i = o; i < d.size() && d[i]; i++) s += static_cast<char>(d[i]);
        return s;
    };
    out = EffectMaterial{};
    out.name = name;
    out.techset = str(u32(52));
    const std::string ts = lower(out.techset);
    if (ts.rfind("effect", 0) != 0 && ts.rfind("particle_cloud", 0) != 0) return false;  // not an effect material
    out.rows = std::max<int>(1, d[10]);
    out.cols = std::max<int>(1, d[11]);
    out.additive = (u32(40) & 0xFF) == 0x22 || ts.find("_add") != std::string::npos;
    out.fog = ts.find("nofog") == std::string::npos;
    out.cloud = ts.rfind("particle_cloud", 0) == 0;
    const uint16_t texCount = u16(48), constCount = u16(50);
    const uint32_t texTable = u32(56), constTable = u32(60);
    for (uint16_t i = 0; i < texCount; i++) {
        const size_t e = texTable + 12ull * i;
        if (str(u32(e)) == "colorMap") out.image = str(u32(e + 8));
    }
    for (uint16_t i = 0; i < constCount; i++) {
        const size_t e = constTable + 20ull * i;
        if (str(u32(e)) == "featherParms") {
            float v[4];
            std::memcpy(v, d.data() + std::min(d.size() - 16, e + 4), 16);
            out.feather = v[1] > 0 ? v[1] : (v[0] > 0 ? 1.0f / v[0] : 0.0f);
        }
    }
    if (out.feather == 0 && ts.find("zfeather") != std::string::npos) out.feather = 10;
    return !out.image.empty();
}

bool Install::materialTechset(const std::string& name, std::string& techset) const {
    if (!valid_) return false;
    std::vector<uint8_t> d;
    if (!readAll(std::filesystem::path(root_) / "raw" / "materials" / name, d) || d.size() < 64) return false;
    uint32_t o;
    std::memcpy(&o, d.data() + 52, 4);
    techset.clear();
    for (size_t i = o; i < d.size() && d[i]; i++) techset += static_cast<char>(d[i]);
    return !techset.empty();
}

void Install::buildIndex() {
    indexed_ = true;
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> iwds;
    for (const auto& e : fs::directory_iterator(fs::path(root_) / "main", ec))
        if (lower(e.path().extension().string()) == ".iwd") iwds.push_back(e.path());
    std::sort(iwds.begin(), iwds.end());  // later archives override earlier ones, as in the game
    for (const fs::path& p : iwds) {
        mz_zip_archive zip{};
        if (!mz_zip_reader_init_file(&zip, p.string().c_str(), 0)) continue;
        const unsigned n = mz_zip_reader_get_num_files(&zip);
        for (unsigned i = 0; i < n; i++) {
            char fname[512];
            mz_zip_reader_get_filename(&zip, i, fname, sizeof fname);
            std::string f = lower(fname);
            if (f.rfind("images/", 0) == 0 && f.size() > 11 && f.compare(f.size() - 4, 4, ".iwi") == 0)
                index_[f.substr(7, f.size() - 11)] = {p.string(), i};
        }
        mz_zip_reader_end(&zip);
    }
}

bool Install::image(const std::string& name, Image& out, std::string& error) {
    if (!valid_) {
        error = "no Black Ops install";
        return false;
    }
    if (!indexed_) buildIndex();
    auto it = index_.find(lower(name));
    if (it == index_.end()) {
        error = "image " + name + " isn't in Black Ops' IWDs";
        return false;
    }
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, it->second.first.c_str(), 0)) {
        error = "can't open " + it->second.first;
        return false;
    }
    size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(&zip, it->second.second, &size, 0);
    mz_zip_reader_end(&zip);
    if (!data) {
        error = "can't read " + name + " from " + it->second.first;
        return false;
    }
    IwiImage iwi;
    const bool ok = parseIwi(static_cast<const uint8_t*>(data), size, 1024, iwi, error);
    mz_free(data);
    if (!ok) return false;
    const IwiLevel& top = iwi.levels.front();
    out = Image{};
    out.width = top.width;
    out.height = top.height;
    out.pixels = top.data;
    switch (iwi.format) {
        case IwiFormat::Dxt1: out.format = 0x31545844; break;  // 'DXT1'
        case IwiFormat::Dxt3: out.format = 0x33545844; break;
        case IwiFormat::Dxt5: out.format = 0x35545844; break;
        default: out.format = 0x15; break;  // expanded to BGRA: A8R8G8B8
    }
    return true;
}

}  // namespace bo1
