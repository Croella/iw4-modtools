#include "dll/volumetric.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "dll/postshaders.h"
#include "light/gradevolume.h"

namespace volumetric {
namespace {

LogFn g_log = nullptr;
void log(const std::string& s) {
    if (g_log) g_log("[volumetric] " + s);
}

// ---- settings ("<map>.volumetric": "key value..." lines) ----------------------------------------
struct Settings {
    bool enabled = false;
    float sun[3] = {0, 0, 1};      // unit vector towards the sun (world space)
    float color[3] = {1, 0.95f, 0.85f};
    float intensity = 0.7f;
    float density = 0.9f;          // how far towards the sun each pixel gathers (1 = all the way)
    float decay = 0.97f;           // per-tap falloff along the ray
    float threshold = 0.25f;       // sky brightness that starts contributing
    float skyWeight = 1.0f;
    float glowRadius = 0.08f;      // sun glow radius (fraction of the screen height)
    float glowStrength = 1.5f;
    int skyMode = -1;              // -1 auto (depth when found), 0 brightness, 1 depth
    float skyDepth = 30000;        // floatz at or beyond this (or <= 0) is sky
    int debug = 0;                 // 1 mask, 2 depth, 3 fog amount
    bool shafts = true;
    // Height fog (off unless "fog 1").
    bool fog = false;
    float fogDensity = 0.0003f;    // extinction per unit at the base height
    float fogFalloff = 0.004f;     // 1/units: how quickly it thins with height
    float fogHeight = 0;           // world z of the base height
    float fogColor[3] = {0.55f, 0.6f, 0.68f};
    float fogSun = 0.6f;           // sun scattering strength (times the sun colour)
    float fogG = 0.6f;             // forward-scattering anisotropy (0 = even, 0.9 = tight glow)
    float fogStart = 64;           // no fog closer than this
    float fogMax = 0.9f;           // max opacity
    float fogSky = 1;              // multiplier on the sky
    float fogSkyDist = 8000;       // distance the sky counts as
    // Sun occluders: world-space triangles (9 floats each) from the map's player clip brushes, drawn
    // black into the shaft mask (the game doesn't draw them, so the sky shows through).
    std::shared_ptr<const std::vector<float>> occ;
    // Ambient occlusion (off unless "ssao 1").
    bool ssao = false;
    float ssaoRadius = 64, ssaoIntensity = 1.5f, ssaoBias = 0.1f, ssaoNear = 20, ssaoFar = 3000;
    // Contact shadows (off unless "contact 1"): short screen-space shadows towards the sun.
    bool contact = false;
    float contactIntensity = 0.6f, contactLength = 32, contactThickness = 24, contactDistance = 2500, contactNear = 20;
    // Colour grading: a baked RGB8 table ("lut <size>" + size*size "lutrow <b> <g> <hex rgb...>" lines).
    bool grade = false;
    float vignette = 0;
    int lutSize = 0;
    std::shared_ptr<const std::vector<uint8_t>> lut;
    // trigger_grade volumes: their own tables, blended over the base by the camera position.
    struct GradeVol {
        std::vector<gradevol::Brush> brushes;
        float blend = 64, blendTime = 0, vignette = 0;
        std::vector<uint8_t> lut;
        int rows = 0;
    };
    std::shared_ptr<const std::vector<GradeVol>> vols;
    // Depth of field.
    bool dof = false, dofAuto = false;
    float dofNear[2] = {0, 0}, dofFar[2] = {1000, 4000}, dofBlur[2] = {6, 4}, dofWeapon = 24, dofAutoSpeed = 4;
    // Sun flare ("flare 1") and glare ("glare 1": bloom of bright pixels + veiling glare near the sun).
    bool flare = false, glare = false;
    float flareIntensity = 1, flareHalo = 1, flareGhosts = 1, flareStreak = 0.5f;
    float flareColor[3] = {1, 0.95f, 0.85f};
    float glareThreshold = 0.8f, glareIntensity = 0.6f, glareRadius = 24, glareSun = 0.3f;
    // Screen-space weather: 0 off, 1 rain, 2 snow; trigger_weather volumes scale it by the camera position.
    int weather = 0;
    float wxIntensity = 1, wxSpeed = 1, wxDistance = 1200, wxWind[2] = {0, 0}, wxColor[3] = {0.7f, 0.74f, 0.8f}, wxOpacity = 0.35f;
    struct WxVol {
        std::vector<gradevol::Brush> brushes;
        float blend = 64, blendTime = 0.5f, intensity = 0;
    };
    std::shared_ptr<const std::vector<WxVol>> wxVols;
    // Local light shafts ("lights 1" + "llight" lines).
    bool lights = false;
    float lightsIntensity = 1, lightsShadows = 0.8f, lightsDistance = 4000;
    int lightsMax = 8;
    struct Light {
        float pos[3], color[3], radius, dir[3], cosOuter, cosInner, flicker;
    };
    std::shared_ptr<const std::vector<Light>> lightList;
};

std::mutex g_mx;
Settings g_settings;               // guarded by g_mx
std::string g_cfgPath;             // guarded by g_mx
std::string g_activeMap;           // guarded by g_mx
FILETIME g_cfgTime{};

bool readSettings(const std::string& path, Settings& s) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    s = Settings{};
    s.enabled = true;
    auto occ = std::make_shared<std::vector<float>>();
    std::shared_ptr<std::vector<uint8_t>> lut;
    int lutRows = 0;
    auto vols = std::make_shared<std::vector<Settings::GradeVol>>();
    auto wxVols = std::make_shared<std::vector<Settings::WxVol>>();
    auto lightList = std::make_shared<std::vector<Settings::Light>>();
    auto nib = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    // Reads n*3 hex bytes into row; false on a short or bad line.
    auto hexRow = [&](const char* h, uint8_t* row, int n) {
        for (int i = 0; i < n * 3; i++) {
            const int hi = nib(h[i * 2]), lo = hi < 0 ? -1 : nib(h[i * 2 + 1]);
            if (hi < 0 || lo < 0) return false;
            row[i] = static_cast<uint8_t>(hi * 16 + lo);
        }
        return true;
    };
    char line[1024];
    while (std::fgets(line, sizeof line, f)) {
        int lb = 0, lg = 0, at = 0, vi = -1;
        if (std::sscanf(line, " vollutrow %d %d %d %n", &vi, &lb, &lg, &at) == 3 && at > 0) {
            const int n = s.lutSize;
            if (vi >= 0 && vi < int(vols->size()) && lb >= 0 && lb < n && lg >= 0 && lg < n) {
                Settings::GradeVol& gv = (*vols)[vi];
                if (gv.lut.size() == static_cast<size_t>(n) * n * n * 3 && hexRow(line + at, &gv.lut[((static_cast<size_t>(lb) * n + lg) * n) * 3], n))
                    gv.rows++;
            }
            continue;
        }
        float gb[3];
        if (std::sscanf(line, " gradevolume %d %f %f %f", &vi, &gb[0], &gb[1], &gb[2]) == 4) {
            if (vi >= 0 && vi < 16 && s.lutSize >= 2) {
                if (int(vols->size()) <= vi) vols->resize(vi + 1);
                Settings::GradeVol& gv = (*vols)[vi];
                gv.blend = std::max(0.0f, gb[0]), gv.blendTime = std::max(0.0f, gb[1]), gv.vignette = std::max(0.0f, std::min(1.0f, gb[2]));
                gv.lut.assign(static_cast<size_t>(s.lutSize) * s.lutSize * s.lutSize * 3, 0);
            }
            continue;
        }
        if (std::sscanf(line, " wxvolume %d %f %f %f", &vi, &gb[0], &gb[1], &gb[2]) == 4) {
            if (vi >= 0 && vi < 16) {
                if (int(wxVols->size()) <= vi) wxVols->resize(vi + 1);
                Settings::WxVol& wv = (*wxVols)[vi];
                wv.blend = std::max(0.0f, gb[0]), wv.blendTime = std::max(0.0f, gb[1]), wv.intensity = std::max(0.0f, gb[2]);
            }
            continue;
        }
        if (std::sscanf(line, " wxbrush %d", &vi) == 1) {
            if (vi >= 0 && vi < int(wxVols->size())) (*wxVols)[vi].brushes.emplace_back();
            continue;
        }
        float wp[4];
        if (std::sscanf(line, " wxplane %d %f %f %f %f", &vi, &wp[0], &wp[1], &wp[2], &wp[3]) == 5) {
            if (vi >= 0 && vi < int(wxVols->size()) && !(*wxVols)[vi].brushes.empty())
                (*wxVols)[vi].brushes.back().push_back({{wp[0], wp[1], wp[2]}, wp[3]});
            continue;
        }
        if (std::sscanf(line, " volbrush %d", &vi) == 1) {
            if (vi >= 0 && vi < int(vols->size())) (*vols)[vi].brushes.emplace_back();
            continue;
        }
        float pl[4];
        if (std::sscanf(line, " volplane %d %f %f %f %f", &vi, &pl[0], &pl[1], &pl[2], &pl[3]) == 5) {
            if (vi >= 0 && vi < int(vols->size()) && !(*vols)[vi].brushes.empty())
                (*vols)[vi].brushes.back().push_back({{pl[0], pl[1], pl[2]}, pl[3]});
            continue;
        }
        if (std::sscanf(line, " lutrow %d %d %n", &lb, &lg, &at) == 2 && at > 0) {
            const int n = s.lutSize;
            if (lut && lb >= 0 && lb < n && lg >= 0 && lg < n) {
                if (hexRow(line + at, &(*lut)[((static_cast<size_t>(lb) * n + lg) * n) * 3], n)) lutRows++;
            }
            continue;
        }
        Settings::Light lt{};
        if (std::sscanf(line, " llight %f %f %f %f %f %f %f %f %f %f %f %f %f", &lt.pos[0], &lt.pos[1], &lt.pos[2], &lt.color[0], &lt.color[1],
                        &lt.color[2], &lt.radius, &lt.dir[0], &lt.dir[1], &lt.dir[2], &lt.cosOuter, &lt.cosInner, &lt.flicker) == 13) {
            if (lt.radius > 1 && lightList->size() < 4096) lightList->push_back(lt);
            continue;
        }
        float t[9];
        if (std::sscanf(line, " occluder %f %f %f %f %f %f %f %f %f", &t[0], &t[1], &t[2], &t[3], &t[4], &t[5], &t[6], &t[7], &t[8]) == 9) {
            occ->insert(occ->end(), t, t + 9);
            continue;
        }
        char key[64] = "";
        float v[4] = {0, 0, 0, 0};
        int n = std::sscanf(line, " %63s %f %f %f %f", key, &v[0], &v[1], &v[2], &v[3]);
        if (n < 2 || key[0] == '/' || key[0] == '#') continue;
        auto is = [&](const char* k) { return _stricmp(key, k) == 0; };
        if (is("enabled")) s.enabled = v[0] != 0;
        else if (is("sun") && n == 4) std::memcpy(s.sun, v, 3 * sizeof(float));
        else if (is("color") && n == 4) std::memcpy(s.color, v, 3 * sizeof(float));
        else if (is("intensity")) s.intensity = v[0];
        else if (is("density")) s.density = v[0];
        else if (is("decay")) s.decay = v[0];
        else if (is("threshold")) s.threshold = v[0];
        else if (is("skyweight")) s.skyWeight = v[0];
        else if (is("glowradius")) s.glowRadius = v[0];
        else if (is("glow")) s.glowStrength = v[0];
        else if (is("skymode")) s.skyMode = static_cast<int>(v[0]);
        else if (is("skydepth")) s.skyDepth = v[0];
        else if (is("debug")) s.debug = static_cast<int>(v[0]);
        else if (is("shafts")) s.shafts = v[0] != 0;
        else if (is("fog")) s.fog = v[0] != 0;
        else if (is("fogdensity")) s.fogDensity = v[0];
        else if (is("fogfalloff")) s.fogFalloff = v[0];
        else if (is("fogheight")) s.fogHeight = v[0];
        else if (is("fogcolor") && n == 4) std::memcpy(s.fogColor, v, 3 * sizeof(float));
        else if (is("fogsun")) s.fogSun = v[0];
        else if (is("fogg")) s.fogG = std::max(-0.95f, std::min(0.95f, v[0]));
        else if (is("fogstart")) s.fogStart = v[0];
        else if (is("fogmax")) s.fogMax = v[0];
        else if (is("fogsky")) s.fogSky = v[0];
        else if (is("fogskydist")) s.fogSkyDist = v[0];
        else if (is("ssao")) s.ssao = v[0] != 0;
        else if (is("ssaoradius")) s.ssaoRadius = std::max(1.0f, v[0]);
        else if (is("ssaointensity")) s.ssaoIntensity = std::max(0.0f, v[0]);
        else if (is("ssaobias")) s.ssaoBias = std::max(0.0f, std::min(0.95f, v[0]));
        else if (is("ssaonear")) s.ssaoNear = std::max(0.0f, v[0]);
        else if (is("ssaofar")) s.ssaoFar = std::max(1.0f, v[0]);
        else if (is("contact")) s.contact = v[0] != 0;
        else if (is("contactparm") && n >= 5)
            s.contactIntensity = std::max(0.0f, std::min(1.0f, v[0])), s.contactLength = std::max(1.0f, std::min(512.0f, v[1])),
            s.contactThickness = std::max(1.0f, v[2]), s.contactDistance = std::max(64.0f, v[3]);
        else if (is("contactnear")) s.contactNear = std::max(0.0f, v[0]);
        else if (is("grade")) s.grade = v[0] != 0;
        else if (is("dof")) s.dof = v[0] != 0;
        else if (is("dofnear") && n >= 3) s.dofNear[0] = std::max(0.0f, v[0]), s.dofNear[1] = std::max(0.0f, v[1]);
        else if (is("doffar") && n >= 3) s.dofFar[0] = std::max(0.0f, v[0]), s.dofFar[1] = std::max(v[0], v[1]);
        else if (is("dofblur") && n >= 3) s.dofBlur[0] = std::max(0.0f, std::min(32.0f, v[0])), s.dofBlur[1] = std::max(0.0f, std::min(32.0f, v[1]));
        else if (is("dofweapon")) s.dofWeapon = std::max(0.0f, v[0]);
        else if (is("dofauto")) s.dofAuto = v[0] != 0;
        else if (is("dofautospeed")) s.dofAutoSpeed = std::max(0.01f, v[0]);
        else if (is("flare")) s.flare = v[0] != 0;
        else if (is("flareparm") && n >= 5)
            s.flareIntensity = std::max(0.0f, v[0]), s.flareHalo = std::max(0.0f, v[1]), s.flareGhosts = std::max(0.0f, v[2]),
            s.flareStreak = std::max(0.0f, v[3]);
        else if (is("flarecolor") && n >= 4) std::memcpy(s.flareColor, v, 3 * sizeof(float));
        else if (is("glare")) s.glare = v[0] != 0;
        else if (is("glareparm") && n >= 5)
            s.glareThreshold = std::max(0.0f, std::min(0.98f, v[0])), s.glareIntensity = std::max(0.0f, v[1]),
            s.glareRadius = std::max(1.0f, std::min(64.0f, v[2])), s.glareSun = std::max(0.0f, v[3]);
        else if (is("weather")) s.weather = std::max(0, std::min(2, static_cast<int>(v[0])));
        else if (is("weatherparm") && n >= 4)
            s.wxIntensity = std::max(0.0f, v[0]), s.wxSpeed = std::max(0.0f, v[1]), s.wxDistance = std::max(32.0f, v[2]);
        else if (is("weatherwind") && n >= 3) s.wxWind[0] = v[0], s.wxWind[1] = v[1];
        else if (is("weathercolor") && n >= 5)
            std::memcpy(s.wxColor, v, 3 * sizeof(float)), s.wxOpacity = std::max(0.0f, std::min(1.0f, v[3]));
        else if (is("lights")) s.lights = v[0] != 0;
        else if (is("lightsparm") && n >= 5)
            s.lightsIntensity = std::max(0.0f, v[0]), s.lightsShadows = std::max(0.0f, std::min(1.0f, v[1])),
            s.lightsMax = std::max(1, std::min(16, static_cast<int>(v[2]))), s.lightsDistance = std::max(64.0f, v[3]);
        else if (is("vignette")) s.vignette = std::max(0.0f, std::min(1.0f, v[0]));
        else if (is("lut")) {
            const int n = static_cast<int>(v[0]);
            if (n >= 2 && n <= 64) {
                s.lutSize = n;
                lut = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(n) * n * n * 3, 0);
                lutRows = 0;
            }
        }
    }
    std::fclose(f);
    if (!occ->empty()) s.occ = std::move(occ);
    if (lut && lutRows == s.lutSize * s.lutSize) s.lut = std::move(lut);
    else s.lutSize = 0;
    if (!s.lut) s.grade = false;
    // Keep only complete volumes.
    vols->erase(std::remove_if(vols->begin(), vols->end(),
                               [&](const Settings::GradeVol& gv) { return !s.lut || gv.rows != s.lutSize * s.lutSize || gv.brushes.empty(); }),
                vols->end());
    if (!vols->empty()) s.vols = std::move(vols);
    wxVols->erase(std::remove_if(wxVols->begin(), wxVols->end(), [](const Settings::WxVol& wv) { return wv.brushes.empty(); }), wxVols->end());
    if (!wxVols->empty()) s.wxVols = std::move(wxVols);
    if (!lightList->empty()) s.lightList = std::move(lightList);
    else s.lights = false;
    float l = std::sqrt(s.sun[0] * s.sun[0] + s.sun[1] * s.sun[1] + s.sun[2] * s.sun[2]);
    if (l > 1e-6f)
        for (float& c : s.sun) c /= l;
    return true;
}

bool fileTime(const std::string& path, FILETIME& t) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a)) return false;
    t = a.ftLastWriteTime;
    return true;
}

std::string lowerBase(const std::string& p) {
    size_t s = p.find_last_of("\\/");
    std::string b = s == std::string::npos ? p : p.substr(s + 1);
    for (char& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (b.size() > 3 && b.compare(b.size() - 3, 3, ".ff") == 0) b.resize(b.size() - 3);
    return b;
}

// ---- camera: GfxViewParms {view, projection, viewProjection, inverseViewProjection, camera} ------
struct ViewParms {
    float v[16], p[16], vp[16];
};
std::atomic<uintptr_t> g_camAddr{0};
// Every camera copy found in the exe image (always mapped, so the constant hook can read them
// directly). The game keeps several (front end / back end), a frame apart while the view moves.
constexpr int kMaxCams = 8;
uintptr_t g_cams[kMaxCams];
std::atomic<int> g_camCount{0};
// The view-projection the GPU got this frame, matched in SetVertexShaderConstantF (render thread).
float g_frameVP[16];
float g_frameEye[3];  // that copy's camera origin (the matrices are camera-relative)
constexpr size_t kOriginOfs = 64 * 4;  // GfxViewParms: 4 matrices, then the camera origin
bool g_frameVPValid = false;
unsigned g_vpMatches = 0, g_vpMisses = 0;
std::atomic<float> g_aspect{0};

bool near1(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

bool plausible(const float* f, float aspect) {
    const float* V = f;
    const float* P = f + 16;
    const float* VP = f + 32;
    if (!(V[15] == 1.0f) || !near1(P[11], 1, 1e-4f)) return false;
    if (V[3] != 0 || V[7] != 0 || V[11] != 0) return false;
    if (P[15] != 0 || P[1] != 0 || P[2] != 0 || P[3] != 0 || P[4] != 0 || P[6] != 0 || P[7] != 0) return false;
    if (!(P[0] > 0.1f && P[0] < 20 && P[5] > 0.1f && P[5] < 20)) return false;
    for (int r = 0; r < 3; r++) {
        for (int c = r; c < 3; c++) {
            float d = V[r * 4] * V[c * 4] + V[r * 4 + 1] * V[c * 4 + 1] + V[r * 4 + 2] * V[c * 4 + 2];
            if (!near1(d, r == c ? 1.0f : 0.0f, 2e-3f)) return false;
        }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += V[r * 4 + k] * P[k * 4 + c];
            float x = VP[r * 4 + c];
            if (!std::isfinite(x) || std::fabs(s - x) > 1e-2f * (1 + std::fabs(x))) return false;
        }
    if (aspect > 0 && std::fabs(P[5] / P[0] - aspect) > 0.06f * aspect) return false;
    return true;
}

bool readSelf(uintptr_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), out, n, &got) && got == n;
}

void scanForCamera() {
    const float aspect = g_aspect.load();
    HMODULE exe = GetModuleHandleA(nullptr);
    uintptr_t exeLo = reinterpret_cast<uintptr_t>(exe), exeHi = exeLo;
    if (IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe))
        exeHi = exeLo + reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(exe) + dos->e_lfanew)->OptionalHeader.SizeOfImage;
    struct Cand {
        uintptr_t at;
        bool image;
    };
    std::vector<Cand> found;
    const size_t kChunk = 1 << 20, kNeed = 48 * 4;
    // Read buffer outside the heap, so the scan can skip it (it holds copies of what it read).
    static uint8_t* buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, kChunk + kNeed, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buf) return;
    const uintptr_t bufLo = reinterpret_cast<uintptr_t>(buf);
    DWORD t0 = GetTickCount();
    uintptr_t a = 0x10000;
    MEMORY_BASIC_INFORMATION m;
    while (VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof m) == sizeof m) {
        uintptr_t lo = reinterpret_cast<uintptr_t>(m.BaseAddress), hi = lo + m.RegionSize;
        a = hi;
        if (hi <= lo) break;
        const DWORD rw = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (m.State != MEM_COMMIT || !(m.Protect & rw) || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) continue;
        const bool image = lo >= exeLo && lo < exeHi;
        if (m.Type == MEM_IMAGE && !image) continue;  // other DLLs
        if (bufLo >= lo && bufLo < hi) continue;
        for (uintptr_t c = lo; c < hi; c += kChunk) {
            size_t n = static_cast<size_t>(std::min<uintptr_t>(kChunk + kNeed, hi - c));
            if (n < kNeed) break;
            if (!readSelf(c, buf, n)) continue;
            for (size_t o = 0; o + kNeed <= n && o < kChunk; o += 4) {
                const float* f = reinterpret_cast<const float*>(buf + o);
                if (f[15] != 1.0f) continue;
                if (plausible(f, aspect)) found.push_back({c + o, image});
            }
        }
    }
    uintptr_t pick = 0;
    for (const Cand& c : found)
        if (c.image) {
            pick = c.at;
            break;
        }
    if (!pick && !found.empty()) pick = found.front().at;
    char line[256];
    std::snprintf(line, sizeof line, "camera scan: %zu candidate(s) in %lu ms (aspect %.3f), using 0x%llX", found.size(),
                  GetTickCount() - t0, aspect, static_cast<unsigned long long>(pick));
    log(line);
    for (size_t i = 0; i < found.size() && i < 12; i++) {
        float cam[3] = {0, 0, 0};
        readSelf(found[i].at + 64 * 4, cam, sizeof cam);
        std::snprintf(line, sizeof line, "  0x%llX %s origin %.1f %.1f %.1f", static_cast<unsigned long long>(found[i].at),
                      found[i].image ? "image" : "heap", cam[0], cam[1], cam[2]);
        log(line);
    }
    int nc = 0;
    for (const Cand& c : found)
        if (c.image && nc < kMaxCams) g_cams[nc++] = c.at;
    g_camCount = 0;
    g_camCount = nc;
    g_camAddr = pick;
}

void scannerThread() {
    for (;;) {
        // Scan while a map with the effect is up and no camera is known (none found yet, or the
        // one found stopped validating in render()).
        bool active;
        {
            std::lock_guard<std::mutex> l(g_mx);
            active = g_settings.enabled && !g_activeMap.empty();
        }
        if (active && g_aspect.load() > 0 && !g_camAddr.load()) scanForCamera();
        Sleep(g_camAddr.load() ? 500 : 3000);
    }
}

// ---- D3D9 hooks ---------------------------------------------------------------------------------
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using SetRenderTarget_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetVSConstF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
using StretchRect_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*,
                                                  D3DTEXTUREFILTERTYPE);
using CreateDSS_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                                  IDirect3DSurface9**, HANDLE*);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                     IDirect3DDevice9**);
constexpr int kReset = 16, kPresent = 17, kCreateDSS = 29, kStretchRect = 34, kSetRenderTarget = 37, kSetVSConstF = 94,
              kCreateDevice = 16;

struct DeviceVt {
    void** vt = nullptr;
    Present_t present = nullptr;
    Reset_t reset = nullptr;
    SetRenderTarget_t setRT = nullptr;
    SetVSConstF_t setVSF = nullptr;
    CreateDSS_t createDSS = nullptr;
    StretchRect_t stretchRect = nullptr;
};
DeviceVt g_vts[16];
// d3d9's own Present/Reset/SetRenderTarget, read from a throwaway device: on current Windows each
// device gets a private heap copy of its vtable, so the game's device is found by these entries.
void* g_realFns[3] = {};
std::atomic<unsigned> g_presents{0};
std::mutex g_vtMx;
CreateDevice_t g_realCreateDevice = nullptr;

const DeviceVt* vtOf(IDirect3DDevice9* d) {
    void** vt = *reinterpret_cast<void***>(d);
    for (const DeviceVt& v : g_vts)
        if (v.vt == vt) return &v;
    return nullptr;
}

bool patchSlot(void** slot, void* hook) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

HRESULT STDMETHODCALLTYPE hkPresent(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
HRESULT STDMETHODCALLTYPE hkSetRenderTarget(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
HRESULT STDMETHODCALLTYPE hkSetVSConstF(IDirect3DDevice9*, UINT, const float*, UINT);
HRESULT STDMETHODCALLTYPE hkCreateDSS(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**,
                                      HANDLE*);
HRESULT STDMETHODCALLTYPE hkStretchRect(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*,
                                        D3DTEXTUREFILTERTYPE);

void hookDeviceVtable(IDirect3DDevice9* d) {
    std::lock_guard<std::mutex> l(g_vtMx);
    void** vt = *reinterpret_cast<void***>(d);
    for (DeviceVt& v : g_vts)
        if (v.vt == vt) return;
    for (DeviceVt& v : g_vts) {
        if (v.vt) continue;
        v.present = reinterpret_cast<Present_t>(vt[kPresent]);
        v.reset = reinterpret_cast<Reset_t>(vt[kReset]);
        v.setRT = reinterpret_cast<SetRenderTarget_t>(vt[kSetRenderTarget]);
        v.setVSF = reinterpret_cast<SetVSConstF_t>(vt[kSetVSConstF]);
        v.createDSS = reinterpret_cast<CreateDSS_t>(vt[kCreateDSS]);
        v.stretchRect = reinterpret_cast<StretchRect_t>(vt[kStretchRect]);
        v.vt = vt;  // published before the slots point at us
        bool ok = patchSlot(&vt[kPresent], reinterpret_cast<void*>(hkPresent)) &&
                  patchSlot(&vt[kReset], reinterpret_cast<void*>(hkReset)) &&
                  patchSlot(&vt[kSetRenderTarget], reinterpret_cast<void*>(hkSetRenderTarget)) &&
                  patchSlot(&vt[kSetVSConstF], reinterpret_cast<void*>(hkSetVSConstF)) &&
                  patchSlot(&vt[kCreateDSS], reinterpret_cast<void*>(hkCreateDSS)) &&
                  patchSlot(&vt[kStretchRect], reinterpret_cast<void*>(hkStretchRect));
        char line[128];
        std::snprintf(line, sizeof line, "hooked device vtable %p (%s)", static_cast<void*>(vt), ok ? "ok" : "FAILED");
        log(line);
        return;
    }
}

// Each hooked IDirect3D9 vtable with its own original CreateDevice (the Steam overlay wraps the
// game's object in its own class, so there can be more than one).
struct D3DVt {
    void** vt = nullptr;
    CreateDevice_t real = nullptr;
};
D3DVt g_d3dVts[8];

HRESULT STDMETHODCALLTYPE hkCreateDevice(IDirect3D9* self, UINT ad, D3DDEVTYPE type, HWND w, DWORD flags,
                                         D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    CreateDevice_t real = g_realCreateDevice;
    void** svt = *reinterpret_cast<void***>(self);
    for (const D3DVt& v : g_d3dVts)
        if (v.vt == svt) real = v.real;
    if (!real) return D3DERR_INVALIDCALL;
    HRESULT hr = real(self, ad, type, w, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        log("game created its device; hooked from the start");
        hookDeviceVtable(*out);
    }
    return hr;
}

// Makes an IDirect3D9 object (the game's) route CreateDevice through us, so a device it creates
// later (startup, vid_restart) is hooked before it makes its depth buffer.
bool hookD3D9Vtable(void** vt) {
    std::lock_guard<std::mutex> l(g_vtMx);
    if (vt[kCreateDevice] == reinterpret_cast<void*>(hkCreateDevice)) return false;
    for (D3DVt& v : g_d3dVts) {
        if (v.vt) continue;
        v.real = reinterpret_cast<CreateDevice_t>(vt[kCreateDevice]);
        v.vt = vt;  // published before the slot points at us
        return patchSlot(&vt[kCreateDevice], reinterpret_cast<void*>(hkCreateDevice));
    }
    return false;
}

void hookD3D9Object(IDirect3D9* d9) {
    if (!d9) return;
    void** vt = *reinterpret_cast<void***>(d9);
    if (vt[kCreateDevice] == reinterpret_cast<void*>(hkCreateDevice)) return;
    const bool ok = hookD3D9Vtable(vt);
    char line[96];
    std::snprintf(line, sizeof line, "hooked the game's Direct3D object %p (%s)", static_cast<void*>(d9), ok ? "ok" : "FAILED");
    log(line);
}

using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
Direct3DCreate9_t g_realCreate9 = nullptr;
IDirect3D9* WINAPI hkDirect3DCreate9(UINT sdk) {
    IDirect3D9* d9 = g_realCreate9(sdk);
    hookD3D9Object(d9);  // whatever class it is (plain d3d9 or the overlay's wrapper)
    return d9;
}

// Points the exe's import of d3d9!Direct3DCreate9 at us (for when we're loaded before the game
// creates its Direct3D object).
void hookCreate9Import() {
    HMODULE exe = GetModuleHandleA(nullptr), d3d = GetModuleHandleA("d3d9.dll");
    if (!exe || !d3d) return;
    void* real = reinterpret_cast<void*>(GetProcAddress(d3d, "Direct3DCreate9"));
    if (!real) return;
    g_realCreate9 = reinterpret_cast<Direct3DCreate9_t>(real);
    auto* base = reinterpret_cast<uint8_t*>(exe);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    for (auto* id = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); id->Name; id++) {
        if (_stricmp(reinterpret_cast<const char*>(base + id->Name), "d3d9.dll") != 0) continue;
        for (auto* t = reinterpret_cast<IMAGE_THUNK_DATA*>(base + id->FirstThunk); t->u1.Function; t++)
            if (reinterpret_cast<void*>(t->u1.Function) == real &&
                patchSlot(reinterpret_cast<void**>(&t->u1.Function), reinterpret_cast<void*>(hkDirect3DCreate9)))
                log("hooked the game's Direct3DCreate9 import");
    }
}

// ---- renderer state (render thread only) --------------------------------------------------------
struct Gpu {
    IDirect3DDevice9* dev = nullptr;
    UINT w = 0, h = 0;
    IDirect3DTexture9* scene = nullptr;
    IDirect3DSurface9* sceneS = nullptr;
    IDirect3DTexture9* half[2] = {};
    IDirect3DSurface9* halfS[2] = {};
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9 *psMask = nullptr, *psBlur = nullptr, *psComp = nullptr, *psDepth = nullptr, *psFog = nullptr;
    IDirect3DVertexShader9* vsOcc = nullptr;
    IDirect3DPixelShader9* psBlack = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    IDirect3DVertexDeclaration9* declOcc = nullptr;  // float3 position
    bool ownScene = false;                           // render() began the scene (at Present), so ends it
    IDirect3DStateBlock9* sb = nullptr;
    IDirect3DTexture9* floatz = nullptr;  // the game's, AddRef'd
    IDirect3DTexture9* depth = nullptr;   // INTZ texture standing in for the game's depth buffer
    IDirect3DTexture9* ao[2] = {};        // full-res AO + blur ping-pong
    IDirect3DSurface9* aoS[2] = {};
    IDirect3DPixelShader9 *psAo = nullptr, *psAoBlur = nullptr, *psGrade = nullptr, *psContact = nullptr;
    IDirect3DPixelShader9 *psDofFocus = nullptr, *psDofDown = nullptr, *psDofBlur = nullptr, *psDofComp = nullptr;
    IDirect3DTexture9* focus[2] = {};  // 1x1 R32F auto-focus distance, ping-pong (this frame / last)
    IDirect3DSurface9* focusS[2] = {};
    int focusCur = 0;
    bool focusValid = false;
    ULONGLONG dofTick = 0;
    IDirect3DVolumeTexture9* lut = nullptr;  // managed pool: survives Reset
    int lutTexSize = 0;
    // Grade volume blending (render thread).
    const void* gradeSetId = nullptr;  // the settings' tables the weights belong to
    std::vector<float> volW, volWUploaded;
    bool lutUploaded = false;
    ULONGLONG gradeTick = 0;
    std::vector<uint8_t> lutWork;
    float vignetteNow = 0;
    // Flare visibility (1x1 R32F, vis + 1, ping-pong) and weather shelter weights.
    IDirect3DPixelShader9 *psFlareVis = nullptr, *psFlare = nullptr, *psGlareDown = nullptr, *psGlareBlur = nullptr, *psGlareComp = nullptr,
                          *psWeather = nullptr;
    IDirect3DTexture9* vis[2] = {};
    IDirect3DSurface9* visS[2] = {};
    int visCur = 0;
    bool visValid = false;
    ULONGLONG flareTick = 0, wxTick = 0;
    const void* wxSetId = nullptr;
    std::vector<float> wxW;
    float wxShelterNow = 1;
    IDirect3DPixelShader9 *psLightScatter = nullptr, *psLightShaft = nullptr, *psLightComp = nullptr;
    IDirect3DTexture9* lacc = nullptr;  // half-res accumulator of the local light shafts
    IDirect3DSurface9* laccS = nullptr;
    unsigned lightFrame = 0;
    bool fogNoDepthLogged = false, aoNoDepthLogged = false;
    bool failed = false;
    unsigned frames = 0;
    unsigned rtLogged = 0;
} g;

template <class T> void rel(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void releaseSized() {
    rel(g.sceneS);
    rel(g.scene);
    for (int i = 0; i < 2; i++) rel(g.halfS[i]), rel(g.half[i]), rel(g.aoS[i]), rel(g.ao[i]), rel(g.focusS[i]), rel(g.focus[i]);
    for (int i = 0; i < 2; i++) rel(g.visS[i]), rel(g.vis[i]);
    rel(g.laccS), rel(g.lacc);
    g.focusValid = g.visValid = false;
    rel(g.sb);
    rel(g.floatz);
    g.w = g.h = 0;
}

bool createShaders(IDirect3DDevice9* d) {
    if (g.vs) return true;
    using namespace postshaders;
    const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
    const D3DVERTEXELEMENT9 el3[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
    bool ok = SUCCEEDED(d->CreateVertexShader(reinterpret_cast<const DWORD*>(volumetric_vsQuad), &g.vs)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psMask), &g.psMask)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psBlur), &g.psBlur)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psComposite), &g.psComp)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psDebugDepth), &g.psDepth)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psFog), &g.psFog)) &&
              SUCCEEDED(d->CreateVertexShader(reinterpret_cast<const DWORD*>(volumetric_vsOccluder), &g.vsOcc)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psBlack), &g.psBlack)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psAo), &g.psAo)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psAoBlur), &g.psAoBlur)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psContact), &g.psContact)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psGrade), &g.psGrade)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psDofFocus), &g.psDofFocus)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psDofDown), &g.psDofDown)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psDofBlur), &g.psDofBlur)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psDofComp), &g.psDofComp)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psFlareVis), &g.psFlareVis)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psFlare), &g.psFlare)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psGlareDown), &g.psGlareDown)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psGlareBlur), &g.psGlareBlur)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psGlareComp), &g.psGlareComp)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psWeather), &g.psWeather)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psLightScatter), &g.psLightScatter)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psLightShaft), &g.psLightShaft)) &&
              SUCCEEDED(d->CreatePixelShader(reinterpret_cast<const DWORD*>(volumetric_psLightComp), &g.psLightComp)) &&
              SUCCEEDED(d->CreateVertexDeclaration(el, &g.decl)) && SUCCEEDED(d->CreateVertexDeclaration(el3, &g.declOcc));
    if (!ok) log("couldn't create the shaders; effect disabled");
    return ok;
}

bool ensureSized(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT bbFmt) {
    if (g.scene && g.w == w && g.h == h) return true;
    IDirect3DTexture9* keepZ = g.floatz;
    g.floatz = nullptr;
    releaseSized();
    g.floatz = keepZ;
    if (FAILED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, bbFmt, D3DPOOL_DEFAULT, &g.scene, nullptr))) return false;
    g.scene->GetSurfaceLevel(0, &g.sceneS);
    for (int i = 0; i < 2; i++) {
        if (FAILED(d->CreateTexture(w / 2, h / 2, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &g.half[i], nullptr)) &&
            FAILED(d->CreateTexture(w / 2, h / 2, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g.half[i], nullptr)))
            return false;
        g.half[i]->GetSurfaceLevel(0, &g.halfS[i]);
    }
    if (FAILED(d->CreateStateBlock(D3DSBT_ALL, &g.sb))) return false;
    g.w = w, g.h = h;
    char line[96];
    std::snprintf(line, sizeof line, "resources for %ux%u", w, h);
    log(line);
    return true;
}

void quad(IDirect3DDevice9* d, UINT tw, UINT th) {
    const float hp[4] = {-1.0f / tw, 1.0f / th, 0, 0};
    d->SetVertexShaderConstantF(0, hp, 1);
    const float v[16] = {-1, 1, 0, 0, 1, 1, 1, 0, -1, -1, 0, 1, 1, -1, 1, 1};
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, 16);
}

void setTarget(IDirect3DDevice9* d, IDirect3DSurface9* s, UINT w, UINT h) {
    d->SetRenderTarget(0, s);
    D3DVIEWPORT9 vp = {0, 0, w, h, 0, 1};
    d->SetViewport(&vp);
}

void sampler(IDirect3DDevice9* d, DWORD s, IDirect3DBaseTexture9* t, bool linear) {
    d->SetTexture(s, t);
    d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetSamplerState(s, D3DSAMP_MINFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    d->SetSamplerState(s, D3DSAMP_MAGFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
}

// Bindings saved by hand on top of the state block: mid-frame (before the HUD) the game keeps
// drawing with what it believes is bound, and it caches its vertex/index buffers, declaration and
// textures. DrawPrimitiveUP also clears stream 0, so without this the game's next HUD draw used our
// quad (a moving box showing the scene).
struct Bindings {
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT vbOffset = 0, vbStride = 0, freq = 1;
    IDirect3DIndexBuffer9* ib = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DBaseTexture9* tex[5] = {};
    float vsc[6 * 4] = {}, psc[41 * 4] = {};
} g_saved;

void saveBindings(IDirect3DDevice9* d) {
    Bindings& b = g_saved;
    d->GetStreamSource(0, &b.vb, &b.vbOffset, &b.vbStride);
    d->GetStreamSourceFreq(0, &b.freq);
    d->GetIndices(&b.ib);
    d->GetVertexDeclaration(&b.decl);
    d->GetVertexShader(&b.vs);
    d->GetPixelShader(&b.ps);
    for (DWORD i = 0; i < 5; i++) d->GetTexture(i, &b.tex[i]);
    d->GetVertexShaderConstantF(0, b.vsc, 6);
    d->GetPixelShaderConstantF(0, b.psc, 41);
}

void restoreBindings(IDirect3DDevice9* d) {
    Bindings& b = g_saved;
    d->SetStreamSource(0, b.vb, b.vbOffset, b.vbStride);
    d->SetStreamSourceFreq(0, b.freq);
    d->SetIndices(b.ib);
    d->SetVertexDeclaration(b.decl);
    d->SetVertexShader(b.vs);
    d->SetPixelShader(b.ps);
    for (DWORD i = 0; i < 5; i++) d->SetTexture(i, b.tex[i]);
    d->SetVertexShaderConstantF(0, b.vsc, 6);
    d->SetPixelShaderConstantF(0, b.psc, 41);
    rel(b.vb);
    rel(b.ib);
    rel(b.decl);
    rel(b.vs);
    rel(b.ps);
    for (auto& t : b.tex) rel(t);
}

void finishRender(IDirect3DDevice9* d, IDirect3DSurface9*& oldRT, IDirect3DSurface9*& oldZ, const D3DVIEWPORT9& oldVp,
                  IDirect3DSurface9* bb) {
    if (g.ownScene) d->EndScene();
    d->SetRenderTarget(0, oldRT);
    d->SetDepthStencilSurface(oldZ);
    d->SetViewport(&oldVp);
    g.sb->Apply();
    restoreBindings(d);
    rel(oldRT);
    rel(oldZ);
    bb->Release();
}

// Solves M x = r for the 3x3 made of VP's clip x, y and w columns (rows of M = those columns).
bool solveClip(const float* VP, const float r[3], float x[3]) {
    const int cols[3] = {0, 1, 3};
    float m[3][3];
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) m[j][i] = VP[i * 4 + cols[j]];
    const float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                      m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (std::fabs(det) < 1e-12f) return false;
    for (int k = 0; k < 3; k++) {  // Cramer
        float a[3][3];
        std::memcpy(a, m, sizeof a);
        for (int j = 0; j < 3; j++) a[j][k] = r[j];
        x[k] = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0])) / det;
    }
    return true;
}

// The eye and the view rays at the screen corners (scaled to view z = 1) into c8..c10; false when
// the matrix can't be solved.
bool viewRays(IDirect3DDevice9* d, const Settings& s, const float* VP, const float camOrigin[3], float eye[3]) {
    // Camera origin: clip x = y = w = 0. Corner rays: clip (x, y, w) = (ndc x, ndc y, 1) with w = 0.
    float r00[3], r10[3], r01[3];
    const float o[3] = {-VP[12], -VP[13], -VP[15]};
    const float c00[3] = {-1, 1, 1}, c10[3] = {1, 1, 1}, c01[3] = {-1, -1, 1};
    if (!solveClip(VP, o, eye) || !solveClip(VP, c00, r00) || !solveClip(VP, c10, r10) || !solveClip(VP, c01, r01)) return false;
    // The game's matrices are camera-relative (eye solves to ~0): the origin is stored beside them.
    if (std::fabs(eye[0]) + std::fabs(eye[1]) + std::fabs(eye[2]) < 1.0f) std::memcpy(eye, camOrigin, 3 * sizeof(float));
    const float cRay0[4] = {r00[0], r00[1], r00[2], s.skyDepth};
    const float cRayX[4] = {r10[0] - r00[0], r10[1] - r00[1], r10[2] - r00[2], 0};
    const float cRayY[4] = {r01[0] - r00[0], r01[1] - r00[1], r01[2] - r00[2], 0};
    d->SetPixelShaderConstantF(8, cRay0, 1);
    d->SetPixelShaderConstantF(9, cRayX, 1);
    d->SetPixelShaderConstantF(10, cRayY, 1);
    return true;
}

// Height fog over the frame (reads the scene copy, writes the backbuffer).
void fogPass(IDirect3DDevice9* d, const Settings& s, const ViewParms& vp, const float camOrigin[3], IDirect3DSurface9* bb) {
    float eye[3];
    if (!viewRays(d, s, vp.vp, camOrigin, eye)) return;
    const bool intz = g.depth != nullptr;
    const float cDepth[4] = {vp.p[10], vp.p[14], intz ? 1.0f : 2.0f, s.fogSkyDist};
    const float cEye[4] = {eye[0], eye[1], eye[2], s.fogStart};
    const float cSunF[4] = {s.sun[0], s.sun[1], s.sun[2], s.fogG};
    const float cCol[4] = {s.fogColor[0], s.fogColor[1], s.fogColor[2], s.fogMax};
    const float cLight[4] = {s.color[0] * s.fogSun, s.color[1] * s.fogSun, s.color[2] * s.fogSun, s.fogSky};
    const float cParm[4] = {s.fogDensity, s.fogFalloff, s.fogHeight, s.debug == 3 ? 1.0f : 0.0f};
    d->SetPixelShaderConstantF(7, cDepth, 1);
    d->SetPixelShaderConstantF(11, cEye, 1);
    d->SetPixelShaderConstantF(12, cSunF, 1);
    d->SetPixelShaderConstantF(13, cCol, 1);
    d->SetPixelShaderConstantF(14, cLight, 1);
    d->SetPixelShaderConstantF(15, cParm, 1);
    setTarget(d, bb, g.w, g.h);
    sampler(d, 0, g.scene, false);
    sampler(d, 2, intz ? static_cast<IDirect3DBaseTexture9*>(g.depth) : g.floatz, false);
    d->SetPixelShader(g.psFog);
    quad(d, g.w, g.h);
    sampler(d, 2, nullptr, false);
    static bool logged = false;
    if (!logged) {
        logged = true;
        char line[200];
        std::snprintf(line, sizeof line, "fog: on (%s depth), eye %.1f %.1f %.1f, P10 %g P14 %g", intz ? "INTZ" : "floatz", eye[0], eye[1],
                      eye[2], vp.p[10], vp.p[14]);
        log(line);
    }
}

// Draws the map's sun occluders black into the current target (the shaft mask).
void drawOccluders(IDirect3DDevice9* d, const Settings& s, const float* VP, const float camOrigin[3], UINT tw, UINT th) {
    if (!s.occ || s.occ->size() < 9) return;
    // The game's matrices are camera-relative (the eye solves to ~0): positions are taken relative to
    // the stored origin. An absolute matrix (the offline test) uses them as they are.
    float eye[3];
    const float o[3] = {-VP[12], -VP[13], -VP[15]};
    float rel[4] = {0, 0, 0, 0};
    if (solveClip(VP, o, eye) && std::fabs(eye[0]) + std::fabs(eye[1]) + std::fabs(eye[2]) < 1.0f)
        rel[0] = camOrigin[0], rel[1] = camOrigin[1], rel[2] = camOrigin[2];
    float cols[16];
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) cols[j * 4 + i] = VP[i * 4 + j];
    const float hp[4] = {-1.0f / tw, 1.0f / th, 0, 0};
    d->SetVertexShaderConstantF(0, hp, 1);
    d->SetVertexShaderConstantF(1, cols, 4);
    d->SetVertexShaderConstantF(5, rel, 1);
    d->SetVertexDeclaration(g.declOcc);
    d->SetVertexShader(g.vsOcc);
    d->SetPixelShader(g.psBlack);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    const UINT tris = static_cast<UINT>(s.occ->size() / 9);
    for (UINT first = 0; first < tris; first += 20000)  // well under any MaxPrimitiveCount
        d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, std::min<UINT>(20000, tris - first), s.occ->data() + first * 9, 12);
    d->SetVertexDeclaration(g.decl);
    d->SetVertexShader(g.vs);
}

// Ambient occlusion: AO into g.ao[0], blur across into g.ao[1], blur down multiplied into `bb`.
bool aoPass(IDirect3DDevice9* d, const Settings& s, const ViewParms& vp, IDirect3DSurface9* bb) {
    IDirect3DBaseTexture9* depthTex = g.depth ? static_cast<IDirect3DBaseTexture9*>(g.depth) : g.floatz;
    if (!depthTex) return false;
    for (int i = 0; i < 2; i++) {
        if (g.ao[i]) continue;
        if (FAILED(d->CreateTexture(g.w, g.h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g.ao[i], nullptr))) {
            log("ssao: couldn't create its render targets; off");
            return false;
        }
        g.ao[i]->GetSurfaceLevel(0, &g.aoS[i]);
    }
    const bool intz = g.depth != nullptr;
    const bool aoOnly = s.debug == 4;
    const float cProj[4] = {vp.p[0], vp.p[5], vp.p[8], vp.p[9]};
    const float cDepth[4] = {vp.p[10], vp.p[14], intz ? 1.0f : 2.0f, s.skyDepth};
    const float cParm[4] = {s.ssaoRadius, s.ssaoIntensity, s.ssaoBias, aoOnly ? 1.0f : 0.0f};
    const float cFade[4] = {s.ssaoNear, s.ssaoFar, 1.0f / g.w, 1.0f / g.h};
    d->SetPixelShaderConstantF(16, cProj, 1);
    d->SetPixelShaderConstantF(17, cDepth, 1);
    d->SetPixelShaderConstantF(18, cParm, 1);
    d->SetPixelShaderConstantF(19, cFade, 1);
    sampler(d, 2, depthTex, false);
    setTarget(d, g.aoS[0], g.w, g.h);
    d->SetPixelShader(g.psAo);
    quad(d, g.w, g.h);
    const float bx[4] = {2.0f / g.w, 0, 0, 0}, by[4] = {0, 2.0f / g.h, 0, 0};
    setTarget(d, g.aoS[1], g.w, g.h);
    sampler(d, 0, g.ao[0], false);
    d->SetPixelShaderConstantF(20, bx, 1);
    d->SetPixelShader(g.psAoBlur);
    quad(d, g.w, g.h);
    setTarget(d, bb, g.w, g.h);
    sampler(d, 0, g.ao[1], false);
    d->SetPixelShaderConstantF(20, by, 1);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, aoOnly ? FALSE : TRUE);  // frame * AO
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
    d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
    quad(d, g.w, g.h);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    sampler(d, 0, nullptr, false);
    sampler(d, 2, nullptr, false);
    static bool logged = false;
    if (!logged) {
        logged = true;
        char line[160];
        std::snprintf(line, sizeof line, "ssao: on (%s depth), radius %g, intensity %g, P0 %g P5 %g", intz ? "INTZ" : "floatz", s.ssaoRadius,
                      s.ssaoIntensity, vp.p[0], vp.p[5]);
        log(line);
    }
    return true;
}

// Contact shadows: a short march towards the sun per pixel (g.ao[0]), blurred across and down
// (reusing psAoBlur) and multiplied into `bb`.
bool contactPass(IDirect3DDevice9* d, const Settings& s, const ViewParms& vp, IDirect3DSurface9* bb) {
    IDirect3DBaseTexture9* depthTex = g.depth ? static_cast<IDirect3DBaseTexture9*>(g.depth) : g.floatz;
    if (!depthTex) return false;
    for (int i = 0; i < 2; i++) {
        if (g.ao[i]) continue;
        if (FAILED(d->CreateTexture(g.w, g.h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g.ao[i], nullptr))) {
            log("contact shadows: couldn't create their render targets; off");
            return false;
        }
        g.ao[i]->GetSurfaceLevel(0, &g.aoS[i]);
    }
    // The sun in view space: the view basis in world space from the corner rays (view z = 1), then
    // solve world = X*ax + Y*ay + Z*fz for (X, Y, Z).
    const float* VP = vp.vp;
    float r00[3], r10[3], r01[3];
    const float c00[3] = {-1, 1, 1}, c10[3] = {1, 1, 1}, c01[3] = {-1, -1, 1};
    if (!solveClip(VP, c00, r00) || !solveClip(VP, c10, r10) || !solveClip(VP, c01, r01)) return false;
    float ax[3], ay[3], fz[3];
    for (int k = 0; k < 3; k++) {
        const float rx = r10[k] - r00[k], ry = r01[k] - r00[k];
        ax[k] = rx * vp.p[0] * 0.5f;
        ay[k] = -ry * vp.p[5] * 0.5f;
        fz[k] = r00[k] + (1 + vp.p[8]) * 0.5f * rx + (1 - vp.p[9]) * 0.5f * ry;
    }
    const float m[3][3] = {{ax[0], ay[0], fz[0]}, {ax[1], ay[1], fz[1]}, {ax[2], ay[2], fz[2]}};
    auto det3 = [](const float a[3][3]) {
        return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
               a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    };
    const float det = det3(m);
    if (std::fabs(det) < 1e-12f) return false;
    float sv[3];
    for (int k = 0; k < 3; k++) {
        float a[3][3];
        std::memcpy(a, m, sizeof a);
        for (int j = 0; j < 3; j++) a[j][k] = s.sun[j];
        sv[k] = det3(a) / det;
    }
    const float sl = std::sqrt(sv[0] * sv[0] + sv[1] * sv[1] + sv[2] * sv[2]);
    if (sl < 1e-6f) return false;
    const bool intz = g.depth != nullptr;
    const bool only = s.debug == 6;
    static unsigned frame = 0;
    frame++;
    const float cProj[4] = {vp.p[0], vp.p[5], vp.p[8], vp.p[9]};
    const float cDepth[4] = {vp.p[10], vp.p[14], intz ? 1.0f : 2.0f, s.skyDepth};
    const float cFade[4] = {s.contactNear, s.contactDistance, 1.0f / g.w, 1.0f / g.h};
    const float cSun[4] = {sv[0] / sl, sv[1] / sl, sv[2] / sl, s.contactLength};
    const float cParm[4] = {s.contactIntensity, s.contactThickness, std::fmod(frame * 0.618034f, 1.0f), only ? 1.0f : 0.0f};
    d->SetPixelShaderConstantF(16, cProj, 1);
    d->SetPixelShaderConstantF(17, cDepth, 1);
    d->SetPixelShaderConstantF(19, cFade, 1);
    d->SetPixelShaderConstantF(39, cSun, 1);
    d->SetPixelShaderConstantF(40, cParm, 1);
    sampler(d, 2, depthTex, false);
    setTarget(d, g.aoS[0], g.w, g.h);
    d->SetPixelShader(g.psContact);
    quad(d, g.w, g.h);
    const float bx[4] = {1.5f / g.w, 0, 0, 0}, by[4] = {0, 1.5f / g.h, 0, 0};
    setTarget(d, g.aoS[1], g.w, g.h);
    sampler(d, 0, g.ao[0], false);
    d->SetPixelShaderConstantF(20, bx, 1);
    d->SetPixelShader(g.psAoBlur);
    quad(d, g.w, g.h);
    setTarget(d, bb, g.w, g.h);
    sampler(d, 0, g.ao[1], false);
    d->SetPixelShaderConstantF(20, by, 1);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, only ? FALSE : TRUE);  // frame * shadow
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
    d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
    quad(d, g.w, g.h);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    sampler(d, 0, nullptr, false);
    sampler(d, 2, nullptr, false);
    static bool logged = false;
    if (!logged) {
        logged = true;
        char line[160];
        std::snprintf(line, sizeof line, "contact shadows: on (%s depth), length %g, intensity %g, view-space sun %.2f %.2f %.2f",
                      intz ? "INTZ" : "floatz", s.contactLength, s.contactIntensity, sv[0] / sl, sv[1] / sl, sv[2] / sl);
        log(line);
    }
    return true;
}

// Uploads an RGB8 table (size^3) into the grade volume texture.
bool uploadLut(IDirect3DDevice9* d, const uint8_t* src, int size) {
    const UINT n = static_cast<UINT>(size);
    if (!g.lut || g.lutTexSize != size) {
        rel(g.lut);
        g.lutTexSize = 0;
        if (FAILED(d->CreateVolumeTexture(n, n, n, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g.lut, nullptr))) {
            log("grade: couldn't create the 3D table; off");
            return false;
        }
        g.lutTexSize = size;
    }
    D3DLOCKED_BOX lb;
    if (FAILED(g.lut->LockBox(0, &lb, nullptr, 0))) return false;
    for (UINT b = 0; b < n; b++)
        for (UINT gg = 0; gg < n; gg++) {
            auto* row = reinterpret_cast<uint8_t*>(lb.pBits) + b * lb.SlicePitch + gg * lb.RowPitch;
            for (UINT r = 0; r < n; r++) {
                const uint8_t* c = src + ((b * n + gg) * n + r) * 3;
                row[r * 4 + 0] = c[2], row[r * 4 + 1] = c[1], row[r * 4 + 2] = c[0], row[r * 4 + 3] = 255;  // BGRA
            }
        }
    g.lut->UnlockBox(0);
    return true;
}

// The frame's grade: the base table with each trigger_grade volume blended over it by how far the
// camera is inside (eased over its blend time). Re-uploads only when the weights move.
bool ensureLut(IDirect3DDevice9* d, const Settings& s, const float eye[3]) {
    if (!s.lut || s.lutSize < 2) return false;
    const size_t nv = s.vols ? s.vols->size() : 0;
    const void* id = s.vols ? static_cast<const void*>(s.vols.get()) : static_cast<const void*>(s.lut.get());
    const ULONGLONG now = GetTickCount64();
    const bool newSet = id != g.gradeSetId || g.volW.size() != nv;
    if (newSet) {
        g.gradeSetId = id;
        g.volW.assign(nv, 0.0f);
        g.volWUploaded.assign(nv, -1.0f);
        g.lutUploaded = false;
        g.gradeTick = now;
    }
    const float dt = std::min(0.25f, (now - g.gradeTick) / 1000.0f);
    g.gradeTick = now;
    bool changed = !g.lutUploaded;
    for (size_t i = 0; i < nv; i++) {
        const Settings::GradeVol& gv = (*s.vols)[i];
        const float target = gradevol::targetWeight(gv.brushes, gv.blend, eye);
        g.volW[i] = newSet ? target : gradevol::approach(g.volW[i], target, gv.blendTime, dt);
        if (std::fabs(g.volW[i] - g.volWUploaded[i]) > 1.0f / 512 || (g.volW[i] == 0) != (g.volWUploaded[i] == 0)) changed = true;
    }
    if (!changed) return true;
    g.lutWork.assign(s.lut->begin(), s.lut->end());
    float vig = s.vignette;
    int active = 0;
    for (size_t i = 0; i < nv; i++) {
        const float w = g.volW[i];
        if (w <= 0) continue;
        gradevol::blendInto(g.lutWork, (*s.vols)[i].lut, w);
        vig += ((*s.vols)[i].vignette - vig) * w;
        active++;
    }
    if (!uploadLut(d, g.lutWork.data(), s.lutSize)) return false;
    g.vignetteNow = vig;
    const bool first = !g.lutUploaded;
    g.lutUploaded = true;
    g.volWUploaded = g.volW;
    if (first) {
        char line[128];
        std::snprintf(line, sizeof line, "grade: %dx%dx%d table uploaded, vignette %g, %zu grade volume(s)", s.lutSize, s.lutSize, s.lutSize,
                      s.vignette, nv);
        log(line);
    }
    static int lastActive = -1;
    if (active != lastActive && nv) {
        lastActive = active;
        log("grade: " + std::to_string(active) + " grade volume(s) active at the camera");
    }
    return true;
}

// Colour grade over the frame (reads the scene copy, writes `bb`).
void gradePass(IDirect3DDevice9* d, const Settings& s, IDirect3DSurface9* bb) {
    const float cGrade[4] = {float(s.lutSize), g.vignetteNow, g_aspect.load(), 0};
    d->SetPixelShaderConstantF(21, cGrade, 1);
    setTarget(d, bb, g.w, g.h);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    sampler(d, 0, g.scene, false);
    d->SetTexture(3, g.lut);
    for (D3DSAMPLERSTATETYPE a : {D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW}) d->SetSamplerState(3, a, D3DTADDRESS_CLAMP);
    d->SetSamplerState(3, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(3, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(3, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(3, D3DSAMP_SRGBTEXTURE, FALSE);
    d->SetPixelShader(g.psGrade);
    quad(d, g.w, g.h);
    d->SetTexture(3, nullptr);
}

// Depth of field: (auto focus) -> half-res colour + blur size -> gather -> composite into `bb`.
bool dofPass(IDirect3DDevice9* d, const Settings& s, const ViewParms& vp, IDirect3DSurface9* bb) {
    IDirect3DBaseTexture9* depthTex = g.depth ? static_cast<IDirect3DBaseTexture9*>(g.depth) : g.floatz;
    if (!depthTex) return false;
    if (s.dofAuto)
        for (int i = 0; i < 2; i++) {
            if (g.focus[i]) continue;
            if (FAILED(d->CreateTexture(1, 1, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &g.focus[i], nullptr))) {
                log("dof: no R32F target for the auto focus; using the fixed distances");
                break;
            }
            g.focus[i]->GetSurfaceLevel(0, &g.focusS[i]);
        }
    const bool autoOn = s.dofAuto && g.focus[0] && g.focus[1];
    const UINT hw = g.w / 2, hh = g.h / 2;
    const float hScale = g.h / 1080.0f * 0.5f;  // blur sizes are full-res pixels at 1080p; the gather runs at half res
    const float cDepth[4] = {vp.p[10], vp.p[14], g.depth ? 1.0f : 2.0f, s.skyDepth};
    const float cDist[4] = {s.dofNear[0], s.dofNear[1], s.dofFar[0], s.dofFar[1]};
    const float cBlur[4] = {s.dofBlur[0] * hScale, s.dofBlur[1] * hScale, s.dofWeapon, s.debug == 5 ? 1.0f : 0.0f};
    const float cStep[4] = {1.0f / hw, 1.0f / hh, 1.0f / g.w, 1.0f / g.h};
    const ULONGLONG now = GetTickCount64();
    const float dt = g.dofTick ? std::min(0.25f, (now - g.dofTick) / 1000.0f) : 0.0f;
    g.dofTick = now;
    const float cAuto[4] = {autoOn ? 1.0f : 0.0f, 1.0f - std::exp(-s.dofAutoSpeed * dt), std::max(s.dofFar[1] * 4, 20000.0f), 0};
    d->SetPixelShaderConstantF(17, cDepth, 1);
    d->SetPixelShaderConstantF(22, cDist, 1);
    d->SetPixelShaderConstantF(23, cBlur, 1);
    d->SetPixelShaderConstantF(24, cStep, 1);
    d->SetPixelShaderConstantF(25, cAuto, 1);
    sampler(d, 2, depthTex, false);
    if (autoOn) {
        const int prev = g.focusCur, cur = 1 - g.focusCur;
        setTarget(d, g.focusS[cur], 1, 1);
        sampler(d, 1, g.focusValid ? g.focus[prev] : nullptr, false);
        if (!g.focusValid) {  // first frame: no history (the shader takes prev <= 0 as none)
            d->SetRenderTarget(0, g.focusS[prev]);
            d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0);
            setTarget(d, g.focusS[cur], 1, 1);
            sampler(d, 1, g.focus[prev], false);
        }
        d->SetPixelShader(g.psDofFocus);
        quad(d, 1, 1);
        g.focusCur = cur;
        g.focusValid = true;
        sampler(d, 4, g.focus[cur], false);
    }
    // half-res colour + blur size
    setTarget(d, g.halfS[0], hw, hh);
    sampler(d, 0, g.scene, true);
    d->SetPixelShader(g.psDofDown);
    quad(d, hw, hh);
    // gather
    setTarget(d, g.halfS[1], hw, hh);
    sampler(d, 0, g.half[0], false);
    d->SetPixelShader(g.psDofBlur);
    quad(d, hw, hh);
    // composite
    setTarget(d, bb, g.w, g.h);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    sampler(d, 0, g.scene, false);
    sampler(d, 1, g.half[1], true);
    d->SetPixelShader(g.psDofComp);
    quad(d, g.w, g.h);
    sampler(d, 1, nullptr, false);
    sampler(d, 2, nullptr, false);
    d->SetTexture(4, nullptr);
    static bool logged = false;
    if (!logged) {
        logged = true;
        char line[200];
        std::snprintf(line, sizeof line, "dof: on (%s depth), near %g-%g, far %g-%g, blur %g/%g px%s", g.depth ? "INTZ" : "floatz", s.dofNear[0],
                      s.dofNear[1], s.dofFar[0], s.dofFar[1], s.dofBlur[0], s.dofBlur[1], autoOn ? ", auto focus" : "");
        log(line);
    }
    return true;
}

// Glare: bright pixels -> half res -> wide blur across and down -> added onto `bb`.
void glarePass(IDirect3DDevice9* d, const Settings& s, IDirect3DSurface9* bb) {
    const UINT hw = g.w / 2, hh = g.h / 2;
    const float step = s.glareRadius * (g.h / 1080.0f) * 0.5f / 6;  // 13 taps span the radius both ways (half-res px)
    float cParm[4] = {s.glareThreshold, s.glareIntensity, 0, 0};
    d->SetPixelShaderConstantF(30, cParm, 1);
    setTarget(d, g.halfS[0], hw, hh);
    sampler(d, 0, g.scene, true);
    d->SetPixelShader(g.psGlareDown);
    quad(d, hw, hh);
    cParm[2] = step / hw, cParm[3] = 0;
    d->SetPixelShaderConstantF(30, cParm, 1);
    setTarget(d, g.halfS[1], hw, hh);
    sampler(d, 0, g.half[0], true);
    d->SetPixelShader(g.psGlareBlur);
    quad(d, hw, hh);
    cParm[2] = 0, cParm[3] = step / hh;
    d->SetPixelShaderConstantF(30, cParm, 1);
    setTarget(d, g.halfS[0], hw, hh);
    sampler(d, 0, g.half[1], true);
    quad(d, hw, hh);
    setTarget(d, bb, g.w, g.h);
    sampler(d, 0, g.half[0], true);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    d->SetPixelShader(g.psGlareComp);
    quad(d, g.w, g.h);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    static bool logged = false;
    if (!logged) {
        logged = true;
        char line[128];
        std::snprintf(line, sizeof line, "glare: on, threshold %g, intensity %g, radius %g px", s.glareThreshold, s.glareIntensity, s.glareRadius);
        log(line);
    }
}

// Sun flare (and the glare's veil near the sun): visibility around the sun (1x1, eased) -> procedural
// flare added onto `bb`. `strength` = on-screen fade; flare off = veil only.
bool flarePass(IDirect3DDevice9* d, const Settings& s, const ViewParms& vp, float su, float sv, float strength, IDirect3DSurface9* bb) {
    for (int i = 0; i < 2; i++) {
        if (g.vis[i]) continue;
        if (FAILED(d->CreateTexture(1, 1, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &g.vis[i], nullptr))) {
            log("flare: no R32F target for the sun visibility; off");
            return false;
        }
        g.vis[i]->GetSurfaceLevel(0, &g.visS[i]);
    }
    IDirect3DBaseTexture9* depthTex = g.depth ? static_cast<IDirect3DBaseTexture9*>(g.depth) : g.floatz;
    const ULONGLONG now = GetTickCount64();
    const float dt = g.flareTick ? std::min(0.25f, (now - g.flareTick) / 1000.0f) : 0.0f;
    g.flareTick = now;
    const float f = s.flare ? 1.0f : 0.0f;
    const float cSun[4] = {su, sv, g_aspect.load(), strength};
    const float cCol[4] = {s.flareColor[0] * s.flareIntensity, s.flareColor[1] * s.flareIntensity, s.flareColor[2] * s.flareIntensity,
                           s.flareGhosts * f};
    const float veil = s.glare ? s.glareSun / std::max(0.01f, s.flareIntensity) : 0.0f;
    const float cParm[4] = {s.flareHalo * f, s.flareStreak * f, veil, 0.012f};
    const float cMisc[4] = {1.0f - std::exp(-14.0f * dt), depthTex ? 0.0f : 1.0f, 0, 0};
    const float cDepth[4] = {vp.p[10], vp.p[14], g.depth ? 1.0f : 2.0f, s.skyDepth};
    d->SetPixelShaderConstantF(17, cDepth, 1);
    d->SetPixelShaderConstantF(26, cSun, 1);
    d->SetPixelShaderConstantF(27, cCol, 1);
    d->SetPixelShaderConstantF(28, cParm, 1);
    d->SetPixelShaderConstantF(29, cMisc, 1);
    sampler(d, 2, depthTex, false);
    const int prev = g.visCur, cur = 1 - g.visCur;
    if (!g.visValid) {  // no history: 0 (the shader takes <= 0.5 as none)
        d->SetRenderTarget(0, g.visS[prev]);
        d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0);
    }
    setTarget(d, g.visS[cur], 1, 1);
    sampler(d, 1, g.vis[prev], false);
    d->SetPixelShader(g.psFlareVis);
    quad(d, 1, 1);
    g.visCur = cur;
    g.visValid = true;
    sampler(d, 1, nullptr, false);
    sampler(d, 2, nullptr, false);
    setTarget(d, bb, g.w, g.h);
    sampler(d, 4, g.vis[cur], false);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
    d->SetPixelShader(g.psFlare);
    quad(d, g.w, g.h);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetTexture(4, nullptr);
    static bool logged = false;
    if (!logged && s.flare) {
        logged = true;
        log(std::string("flare: on (") + (depthTex ? g.depth ? "INTZ" : "floatz" : "no depth: always visible") + ")");
    }
    return true;
}

// How much weather reaches the camera: trigger_weather volumes blended over 1 in order, eased.
float weatherShelter(const Settings& s, const float eye[3]) {
    const size_t nv = s.wxVols ? s.wxVols->size() : 0;
    const ULONGLONG now = GetTickCount64();
    const bool newSet = s.wxVols.get() != g.wxSetId || g.wxW.size() != nv;
    const float dt = newSet || !g.wxTick ? 0.0f : std::min(0.25f, (now - g.wxTick) / 1000.0f);
    g.wxTick = now;
    if (newSet) g.wxSetId = s.wxVols.get(), g.wxW.assign(nv, 0.0f);
    float m = 1;
    for (size_t i = 0; i < nv; i++) {
        const Settings::WxVol& wv = (*s.wxVols)[i];
        const float target = gradevol::targetWeight(wv.brushes, wv.blend, eye);
        g.wxW[i] = newSet ? target : gradevol::approach(g.wxW[i], target, wv.blendTime, dt);
        m += (wv.intensity - m) * g.wxW[i];
    }
    return m;
}

// A light's brightness this frame: 1, or a flicker (a few out-of-step sines) for flickering ones.
float lightFlicker(const Settings::Light& L, size_t index, double secs) {
    if (L.flicker <= 0) return 1;
    const double ph = index * 1.7;
    const double n = 0.5 * std::sin(secs * 13.0 + ph) + 0.3 * std::sin(secs * 7.3 + 2 * ph) + 0.2 * std::sin(secs * 23.0 + 3 * ph);
    return static_cast<float>(1 - L.flicker * (0.5 + 0.5 * n));
}

// Local light shafts: the lights that matter most at the camera, each scattered (half res) then
// shadowed towards its screen position into an accumulator, which is added onto `bb`.
bool lightsPass(IDirect3DDevice9* d, const Settings& s, const ViewParms& vp, const float camOrigin[3], IDirect3DSurface9* bb) {
    IDirect3DBaseTexture9* depthTex = g.depth ? static_cast<IDirect3DBaseTexture9*>(g.depth) : g.floatz;
    float eye[3];
    if (!depthTex || !s.lightList || !viewRays(d, s, vp.vp, camOrigin, eye)) return false;
    const UINT hw = g.w / 2, hh = g.h / 2;
    if (!g.lacc) {
        if (FAILED(d->CreateTexture(hw, hh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &g.lacc, nullptr)) &&
            FAILED(d->CreateTexture(hw, hh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g.lacc, nullptr))) {
            log("lights: couldn't create the accumulator; off");
            return false;
        }
        g.lacc->GetSurfaceLevel(0, &g.laccS);
    }
    // The matrices may be camera-relative (the offline test's aren't): positions go through VP less this.
    float relEye[3] = {0, 0, 0};
    {
        float e0[3];
        const float o[3] = {-vp.vp[12], -vp.vp[13], -vp.vp[15]};
        if (solveClip(vp.vp, o, e0) && std::fabs(e0[0]) + std::fabs(e0[1]) + std::fabs(e0[2]) < 1.0f) std::memcpy(relEye, eye, sizeof relEye);
    }
    // Pick: in front of (or around) the camera, within the distance, brightest at the camera first.
    struct Pick {
        size_t i;
        float score, clip[4];
    };
    std::vector<Pick> picks;
    const auto& list = *s.lightList;
    for (size_t i = 0; i < list.size(); i++) {
        const Settings::Light& L = list[i];
        const float dx = L.pos[0] - eye[0], dy = L.pos[1] - eye[1], dz = L.pos[2] - eye[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist - L.radius > s.lightsDistance) continue;
        Pick pk{i, 0, {0, 0, 0, 0}};
        const float p[3] = {L.pos[0] - relEye[0], L.pos[1] - relEye[1], L.pos[2] - relEye[2]};
        for (int j = 0; j < 4; j++) pk.clip[j] = p[0] * vp.vp[j] + p[1] * vp.vp[4 + j] + p[2] * vp.vp[8 + j] + vp.vp[12 + j];
        if (pk.clip[3] < -L.radius) continue;  // wholly behind the camera
        if (pk.clip[3] > 0) {                  // off to the side by more than its radius (roughly)
            const float sx = std::fabs(pk.clip[0]) / pk.clip[3], sy = std::fabs(pk.clip[1]) / pk.clip[3];
            const float slack = 1 + L.radius / std::max(pk.clip[3], 1.0f) * std::max(vp.p[0], vp.p[5]);
            if (dist > L.radius && (sx > slack || sy > slack)) continue;
        }
        const float lum = L.color[0] * 0.3f + L.color[1] * 0.59f + L.color[2] * 0.11f;
        pk.score = lum * L.radius * L.radius / std::max(dist * dist, L.radius * L.radius);
        picks.push_back(pk);
    }
    std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.score > b.score; });
    if (picks.size() > size_t(s.lightsMax)) picks.resize(s.lightsMax);
    if (picks.empty()) return false;
    const double secs = static_cast<double>(GetTickCount64() % 3600000ull) / 1000.0;
    const float cEye[4] = {eye[0], eye[1], eye[2], 0};
    const float cDepth[4] = {vp.p[10], vp.p[14], g.depth ? 1.0f : 2.0f, s.skyDepth};
    d->SetPixelShaderConstantF(11, cEye, 1);
    d->SetPixelShaderConstantF(17, cDepth, 1);
    d->SetRenderTarget(0, g.laccS);
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0);
    sampler(d, 2, depthTex, false);
    g.lightFrame++;
    for (const Pick& pk : picks) {
        const Settings::Light& L = list[pk.i];
        const float k = 0.0007f * s.lightsIntensity * lightFlicker(L, pk.i, secs);
        const float cPos[4] = {L.pos[0], L.pos[1], L.pos[2], L.radius};
        const float cCol[4] = {L.color[0] * k, L.color[1] * k, L.color[2] * k, 0};
        const float cSpot[4] = {L.dir[0], L.dir[1], L.dir[2], L.cosOuter};
        const bool front = pk.clip[3] > 1;
        const float cMisc[4] = {L.cosInner, s.lightsShadows, std::fmod(g.lightFrame * 0.618034f, 1.0f), front && s.lightsShadows > 0 ? 1.0f : 0.0f};
        const float cScr[4] = {front ? 0.5f + 0.5f * pk.clip[0] / pk.clip[3] : 0.5f, front ? 0.5f - 0.5f * pk.clip[1] / pk.clip[3] : 0.5f, 0.9f, 0};
        d->SetPixelShaderConstantF(34, cPos, 1);
        d->SetPixelShaderConstantF(35, cCol, 1);
        d->SetPixelShaderConstantF(36, cSpot, 1);
        d->SetPixelShaderConstantF(37, cMisc, 1);
        d->SetPixelShaderConstantF(38, cScr, 1);
        setTarget(d, g.halfS[0], hw, hh);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        d->SetPixelShader(g.psLightScatter);
        quad(d, hw, hh);
        setTarget(d, g.laccS, hw, hh);
        sampler(d, 0, g.half[0], true);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        d->SetPixelShader(g.psLightShaft);
        quad(d, hw, hh);
        sampler(d, 0, nullptr, false);
    }
    sampler(d, 2, nullptr, false);
    setTarget(d, bb, g.w, g.h);
    sampler(d, 0, g.lacc, true);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
    d->SetPixelShader(g.psLightComp);
    quad(d, g.w, g.h);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    static size_t lastCount = ~size_t(0);
    if (picks.size() != lastCount) {
        lastCount = picks.size();
        char line[128];
        std::snprintf(line, sizeof line, "lights: %zu of %zu light(s) drawn (%s depth)", picks.size(), list.size(), g.depth ? "INTZ" : "floatz");
        log(line);
    }
    return true;
}

// Rain / snow over the frame (reads the scene copy, writes `bb`).
bool weatherPass(IDirect3DDevice9* d, const Settings& s, const ViewParms& vp, const float camOrigin[3], float shelter, IDirect3DSurface9* bb) {
    IDirect3DBaseTexture9* depthTex = g.depth ? static_cast<IDirect3DBaseTexture9*>(g.depth) : g.floatz;
    float eye[3];
    if (!depthTex || !viewRays(d, s, vp.vp, camOrigin, eye)) return false;
    const bool snow = s.weather == 2;
    const double secs = static_cast<double>(GetTickCount64() % 3600000ull) / 1000.0;
    const float fall = (snow ? 48.0f : 420.0f) * s.wxSpeed;
    const float cDepth[4] = {vp.p[10], vp.p[14], g.depth ? 1.0f : 2.0f, s.skyDepth};
    const float cParm[4] = {float(s.weather), std::min(1.0f, (snow ? 0.45f : 0.5f) * s.wxIntensity), static_cast<float>(secs), s.wxDistance};
    const float cWind[4] = {s.wxWind[0], s.wxWind[1], fall, 2.0f / (std::max(0.1f, vp.p[5]) * g.h)};
    const float cCol[4] = {s.wxColor[0], s.wxColor[1], s.wxColor[2], s.wxOpacity * std::min(1.0f, s.wxIntensity) * shelter};
    d->SetPixelShaderConstantF(17, cDepth, 1);
    d->SetPixelShaderConstantF(31, cParm, 1);
    d->SetPixelShaderConstantF(32, cWind, 1);
    d->SetPixelShaderConstantF(33, cCol, 1);
    setTarget(d, bb, g.w, g.h);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    sampler(d, 0, g.scene, false);
    sampler(d, 2, depthTex, false);
    d->SetPixelShader(g.psWeather);
    quad(d, g.w, g.h);
    sampler(d, 2, nullptr, false);
    static bool logged = false;
    if (!logged) {
        logged = true;
        char line[160];
        std::snprintf(line, sizeof line, "weather: %s, intensity %g, distance %g, %zu shelter volume(s)", snow ? "snow" : "rain", s.wxIntensity,
                      s.wxDistance, s.wxVols ? s.wxVols->size() : size_t(0));
        log(line);
    }
    return true;
}

// Draws the effect onto `bb` (a reference render() releases): the backbuffer at Present, or the
// game's scene target when the game switches to 2D drawing (so the HUD goes on top).
void render(IDirect3DDevice9* d, const Settings& s, IDirect3DSurface9* bb) {
    if (!bb) return;
    if (g.failed || (g.dev && g.dev != d)) {  // only the first device that presents (the game's)
        bb->Release();
        return;
    }
    g.dev = d;
    D3DSURFACE_DESC bd;
    bb->GetDesc(&bd);
    g_aspect = float(bd.Width) / float(bd.Height);
    if (!createShaders(d) || !ensureSized(d, bd.Width, bd.Height, bd.Format)) {
        log("couldn't create render targets; effect disabled");
        g.failed = true;
        bb->Release();
        return;
    }
    // Camera -> sun position on screen.
    const uintptr_t cam = g_camAddr.load();
    ViewParms vp;
    if (!cam || !readSelf(cam, &vp, sizeof vp) || !(vp.v[15] == 1.0f) || !near1(vp.p[11], 1, 1e-4f)) {
        if (cam) g_camAddr = 0;  // rescan
        bb->Release();
        return;
    }
    // Prefer the matrix the GPU actually drew this frame with (the copies are a frame apart while
    // the view moves); fall back to the scanned copy.
    if (g_frameVPValid) {
        std::memcpy(vp.vp, g_frameVP, sizeof vp.vp);
        if (++g_vpMatches == 1) log("camera: using this frame's matrix from the shader constants");
    } else if (++g_vpMisses == 600 && !g_vpMatches) {
        log("camera: no shader-constant match yet; using the scanned copy (may lag a frame)");
    }
    // The sun is a direction: clip.w = view-space z (P[11] = 1, P[15] = 0), so facing comes from VP.
    float clip[4] = {0, 0, 0, 0};
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 3; i++) clip[j] += s.sun[i] * vp.vp[i * 4 + j];
    const float facing = clip[3];
    float fade = 0, su = 0, sv = 0;
    if (facing > 0.02f) {  // else the sun is behind the camera
        const float nx = clip[0] / clip[3], ny = clip[1] / clip[3];
        const float off = std::max(std::fabs(nx), std::fabs(ny));
        fade = std::min(1.0f, facing / 0.35f) * std::max(0.0f, std::min(1.0f, (2.5f - off) / 1.5f));
        if (s.debug == 1 || s.debug == 2) fade = 1;
        su = 0.5f + 0.5f * nx, sv = 0.5f - 0.5f * ny;
    }
    const bool shaftsOn = s.shafts && fade > 0;
    IDirect3DBaseTexture9* fogDepthTex = g.depth ? static_cast<IDirect3DBaseTexture9*>(g.depth) : g.floatz;
    const bool fogOn = s.fog && fogDepthTex;
    if (s.fog && !fogDepthTex && !g.fogNoDepthLogged) {
        g.fogNoDepthLogged = true;
        log("fog: no depth yet (the game made its depth buffer before the DLL loaded); run vid_restart once");
    }
    IDirect3DBaseTexture9* aoDepthTex = g.depth ? static_cast<IDirect3DBaseTexture9*>(g.depth) : g.floatz;
    const bool aoOn = s.ssao && aoDepthTex;
    const bool contactOn = s.contact && aoDepthTex && s.contactIntensity > 0 && (s.debug == 0 || s.debug == 6);
    if (s.ssao && !aoDepthTex && !g.aoNoDepthLogged) {
        g.aoNoDepthLogged = true;
        log("ssao: no depth yet (the game made its depth buffer before the DLL loaded); run vid_restart once");
    }
    float eyeNow[3] = {0, 0, 0};
    if (g_frameVPValid) std::memcpy(eyeNow, g_frameEye, sizeof eyeNow);
    else readSelf(cam + kOriginOfs, eyeNow, sizeof eyeNow);
    const bool gradeOn = s.grade && s.debug == 0 && ensureLut(d, s, eyeNow);
    const bool dofOn = s.dof && aoDepthTex && (s.debug == 0 || s.debug == 5);
    // The flare needs the sun on screen (it fades out as the sun leaves it); the glare's veil rides along.
    float flareFade = 0;
    if (facing > 0.02f) {
        const float nx = su * 2 - 1, ny = 1 - sv * 2, off = std::max(std::fabs(nx), std::fabs(ny));
        flareFade = std::min(1.0f, facing / 0.35f) * std::max(0.0f, std::min(1.0f, (1.15f - off) / 0.15f));
    }
    const bool flareOn = (s.flare || (s.glare && s.glareSun > 0)) && flareFade > 0 && s.debug == 0;
    const bool glareOn = s.glare && s.glareIntensity > 0 && s.debug == 0;
    const float shelter = s.weather ? weatherShelter(s, eyeNow) : 0.0f;
    const bool wxOn = s.weather && aoDepthTex && shelter > 0.001f && s.wxIntensity > 0 && s.debug == 0;
    const bool lightsOn = s.lights && s.lightList && aoDepthTex && s.lightsIntensity > 0 && s.debug == 0;
    if (!shaftsOn && !fogOn && !aoOn && !gradeOn && !dofOn && !flareOn && !glareOn && !wxOn && !lightsOn && !contactOn) {
        bb->Release();
        return;
    }
    // Sky test for the mask: the INTZ depth (always current) beats floatz (only filled when the game
    // needs it), which beats brightness (bright walls would count as sky).
    const bool wantDepth = s.skyMode == 1 || s.skyMode == -1;
    const int maskMode = !wantDepth ? 0 : g.depth ? 2 : g.floatz ? 1 : 0;
    IDirect3DBaseTexture9* maskDepth = maskMode == 2 ? static_cast<IDirect3DBaseTexture9*>(g.depth) : maskMode == 1 ? g.floatz : nullptr;

    // Save what we touch: the state block covers states, shaders, constants and textures; render
    // targets, depth buffer and viewport are saved by hand.
    g.sb->Capture();
    saveBindings(d);
    IDirect3DSurface9 *oldRT = nullptr, *oldZ = nullptr;
    d->GetRenderTarget(0, &oldRT);
    d->GetDepthStencilSurface(&oldZ);
    D3DVIEWPORT9 oldVp;
    d->GetViewport(&oldVp);

    d->StretchRect(bb, nullptr, g.sceneS, nullptr, D3DTEXF_NONE);
    g.ownScene = SUCCEEDED(d->BeginScene());  // fails mid-frame (the game's scene is open): fine
    d->SetDepthStencilSurface(nullptr);
    for (DWORD i = 1; i < 4; i++) d->SetRenderTarget(i, nullptr);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    d->SetVertexDeclaration(g.decl);
    d->SetVertexShader(g.vs);
    const float cSun[4] = {su, sv, g_aspect.load(), std::max(0.005f, s.glowRadius)};
    const float cMask[4] = {float(maskMode), s.skyDepth, s.threshold, s.skyWeight};
    const float cGlow[4] = {s.color[0] * s.glowStrength, s.color[1] * s.glowStrength, s.color[2] * s.glowStrength, 0};
    const float cTint[4] = {s.color[0] * s.intensity, s.color[1] * s.intensity, s.color[2] * s.intensity, fade};
    const float cDbg[4] = {1.0f / std::max(1.0f, s.skyDepth), vp.p[10], vp.p[14], 0};
    d->SetPixelShaderConstantF(1, cSun, 1);
    d->SetPixelShaderConstantF(2, cMask, 1);
    d->SetPixelShaderConstantF(3, cGlow, 1);
    d->SetPixelShaderConstantF(5, cTint, 1);
    d->SetPixelShaderConstantF(6, cDbg, 1);
    const UINT hw = g.w / 2, hh = g.h / 2;
    static int loggedMaskMode = -1;
    if (shaftsOn && loggedMaskMode != maskMode) {
        loggedMaskMode = maskMode;
        static const char* kNames[] = {"brightness", "floatz depth", "INTZ depth"};
        log(std::string("shafts: sky mask from ") + kNames[maskMode] +
            (s.occ ? ", " + std::to_string(s.occ->size() / 9) + " occluder triangle(s)" : std::string(", no occluders")));
    }
    float camOrigin[3] = {0, 0, 0};
    if (g_frameVPValid) std::memcpy(camOrigin, g_frameEye, sizeof camOrigin);
    else readSelf(cam + kOriginOfs, camOrigin, sizeof camOrigin);

    // Order: AO (darkens the lit scene) -> fog (covers it with distance) -> DOF -> local lights -> weather -> shafts ->
    // glare -> flare -> grade (last).
    // Each pass that reads the frame gets a fresh copy of what the one before drew.
    auto recopy = [&] { d->StretchRect(bb, nullptr, g.sceneS, nullptr, D3DTEXF_NONE); };
    auto finish = [&] {
        if (glareOn) glarePass(d, s, bb);
        if (flareOn) flarePass(d, s, vp, su, sv, flareFade, bb);
        if (gradeOn) {
            recopy();
            gradePass(d, s, bb);
        }
        finishRender(d, oldRT, oldZ, oldVp, bb);
    };
    if (aoOn && aoPass(d, s, vp, bb)) {
        if (s.debug == 4) {
            finishRender(d, oldRT, oldZ, oldVp, bb);
            return;
        }
        recopy();
    }
    if (contactOn && contactPass(d, s, vp, bb)) {
        if (s.debug == 6) {
            finishRender(d, oldRT, oldZ, oldVp, bb);
            return;
        }
        recopy();
    }
    if (fogOn) {
        fogPass(d, s, vp, camOrigin, bb);
        recopy();
    }
    if (dofOn && dofPass(d, s, vp, bb)) {
        if (s.debug == 5) {
            finishRender(d, oldRT, oldZ, oldVp, bb);
            return;
        }
        recopy();
    }
    if (lightsOn && lightsPass(d, s, vp, camOrigin, bb)) recopy();
    if (wxOn && weatherPass(d, s, vp, camOrigin, shelter, bb)) recopy();
    if (!shaftsOn) {
        finish();
        return;
    }

    // 1: mask (half res)
    setTarget(d, g.halfS[0], hw, hh);
    sampler(d, 0, g.scene, true);
    sampler(d, 1, maskDepth, false);
    d->SetPixelShader(g.psMask);
    quad(d, hw, hh);
    drawOccluders(d, s, vp.vp, camOrigin, hw, hh);
    // 2 + 3: radial blur, coarse then fine (32 x 32 effective taps)
    const float b1[4] = {s.density, s.decay, 1, 0};
    const float b2[4] = {s.density / 32.0f, 1.0f, 1, 0};
    setTarget(d, g.halfS[1], hw, hh);
    sampler(d, 0, g.half[0], true);
    sampler(d, 1, nullptr, false);
    d->SetPixelShader(g.psBlur);
    d->SetPixelShaderConstantF(4, b1, 1);
    quad(d, hw, hh);
    setTarget(d, g.halfS[0], hw, hh);
    sampler(d, 0, g.half[1], true);
    d->SetPixelShaderConstantF(4, b2, 1);
    quad(d, hw, hh);
    // 4: composite onto the frame
    setTarget(d, bb, g.w, g.h);
    if (s.debug == 2 && g.floatz) {
        sampler(d, 1, g.floatz, false);
        d->SetPixelShader(g.psDepth);
    } else {
        const bool additive = s.debug == 0;
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, additive);
        d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        if (s.debug == 1) {  // the mask itself
            const float one[4] = {1, 1, 1, 1};
            d->SetPixelShaderConstantF(5, one, 1);
            // re-run the mask straight to the screen
            sampler(d, 0, g.scene, true);
            sampler(d, 1, maskDepth, false);
            d->SetPixelShader(g.psMask);
            quad(d, g.w, g.h);
            drawOccluders(d, s, vp.vp, camOrigin, g.w, g.h);
            finishRender(d, oldRT, oldZ, oldVp, bb);
            return;
        } else {
            sampler(d, 0, g.half[0], true);
            d->SetPixelShader(g.psComp);
        }
    }
    quad(d, g.w, g.h);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    if (glareOn) recopy();
    finish();
}

std::atomic<bool> g_inPresent{false};
// Drawing under the HUD: the game draws the world, then switches to an orthographic 2D matrix for
// its HUD/menus. The first such upload after the world's camera matrix is where the effect goes;
// if that never happens (or the 2D goes elsewhere), it falls back to Present (over the HUD).
bool g_drawnThisFrame = false;          // render thread only
IDirect3DSurface9* g_worldRT = nullptr;  // identity only (no reference): the world's colour target
unsigned g_midFrames = 0, g_presentFrames = 0;
int g_diagLeft = 0;                      // > 0: log that many constant uploads (when 2D isn't found)
bool g_otherTargetLogged = false;

bool currentSettings(Settings& s) {
    std::lock_guard<std::mutex> l(g_mx);
    s = g_settings;
    return s.enabled && !g_activeMap.empty();
}

// The 2D pass's matrix: pixels -> clip (x scale 2/width, y scale -2/height, offset -1, +1), in
// either layout.
bool isOrtho2D(const float* m) {
    auto chk = [](const float* a) {
        auto z = [](float x) { return std::fabs(x) < 1e-6f; };
        return z(a[3]) && z(a[7]) && z(a[11]) && std::fabs(a[15] - 1) < 1e-3f && z(a[1]) && z(a[4]) && a[0] > 0 && a[0] < 0.05f &&
               a[5] < 0 && a[5] > -0.05f && std::fabs(a[12] + 1) < 0.05f && std::fabs(a[13] - 1) < 0.05f;
    };
    float t[16];
    for (int i = 0; i < 16; i++) t[i] = m[(i & 3) * 4 + (i >> 2)];
    return chk(m) || chk(t);
}

// The game is about to draw 2D: the world is finished in the current target.
// Draws the effect into `target` (the world's colour target) mid-frame. Triggered by the game
// copying that target out (StretchRect: its post-effects then draw the copy back over the screen,
// so the effect must already be in it) or, if it never copies, by its switch to 2D drawing.
bool g_copyTrigger = false;  // the game copies the world target out: only that triggers from now on

void renderBeforeHud(IDirect3DDevice9* d, IDirect3DSurface9* target, bool fromCopy) {
    Settings s;
    if (!currentSettings(s) || !target) return;
    if (target != g_worldRT) {
        if (!fromCopy && !g_otherTargetLogged) {
            g_otherTargetLogged = true;
            log("hud: the game's 2D goes to another target than the world; drawing at Present instead");
        }
        return;
    }
    IDirect3DSurface9* bb = nullptr;
    D3DSURFACE_DESC rd{}, bd{};
    target->GetDesc(&rd);
    if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        bb->GetDesc(&bd);
        bb->Release();
    }
    if (rd.Width != bd.Width || rd.Height != bd.Height) return;
    target->AddRef();  // render() releases it
    g_inPresent = true;
    render(d, s, target);
    g_inPresent = false;
    g_drawnThisFrame = true;
    if (++g_midFrames == 1)
        log(fromCopy ? "hud: drawing before the game copies the frame for its post effects (the HUD stays on top)"
                     : "hud: drawing before the game's 2D pass (the HUD stays on top)");
}

// Everything done once per presented frame, before the real Present.
void onFrame(IDirect3DDevice9* d) {
    g_presents++;
    if (++g.frames == 1) log("first Present seen");
    if (g.frames % 60 == 0) {  // live tuning: re-read when the file changes
        std::lock_guard<std::mutex> l(g_mx);
        FILETIME t;
        if (!g_cfgPath.empty() && fileTime(g_cfgPath, t) && CompareFileTime(&t, &g_cfgTime) != 0) {
            g_cfgTime = t;
            if (readSettings(g_cfgPath, g_settings)) log("reloaded " + g_cfgPath);
        }
    }
    Settings s;
    const bool active = currentSettings(s);
    if (active && !g_drawnThisFrame && !g_inPresent.exchange(true)) {
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) render(d, s, bb);
        g_inPresent = false;
        if (g_frameVPValid && ++g_presentFrames == 300 && !g_midFrames) {
            log("hud: no switch to 2D seen after the world; drawing at Present (over the HUD). Next frame's uploads:");
            g_diagLeft = 48;
        }
    }
    g_drawnThisFrame = false;
    g_frameVPValid = false;  // the next frame's draws set it again
    g_worldRT = nullptr;
}

HRESULT STDMETHODCALLTYPE hkPresent(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    const DeviceVt* v = vtOf(d);
    if (!v) return D3DERR_INVALIDCALL;
    onFrame(d);
    return v->present(d, a, b, w, r);
}

// The game presents through its swap chain (IDirect3DSwapChain9::Present, slot 3), not the device.
using SwapPresent_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
struct SwapVt {
    void** vt = nullptr;
    SwapPresent_t present = nullptr;
};
SwapVt g_swapVts[16];

HRESULT STDMETHODCALLTYPE hkSwapPresent(IDirect3DSwapChain9* sc, const RECT* a, const RECT* b, HWND w, const RGNDATA* r, DWORD f) {
    void** vt = *reinterpret_cast<void***>(sc);
    SwapPresent_t real = nullptr;
    for (const SwapVt& v : g_swapVts)
        if (v.vt == vt) real = v.present;
    if (!real) return D3DERR_INVALIDCALL;
    IDirect3DDevice9* d = nullptr;
    if (SUCCEEDED(sc->GetDevice(&d)) && d) {
        onFrame(d);
        d->Release();
    }
    return real(sc, a, b, w, r, f);
}

void hookSwapChains(IDirect3DDevice9* d) {
    std::lock_guard<std::mutex> l(g_vtMx);
    for (UINT i = 0, n = d->GetNumberOfSwapChains(); i < n; i++) {
        IDirect3DSwapChain9* sc = nullptr;
        if (FAILED(d->GetSwapChain(i, &sc)) || !sc) continue;
        void** vt = *reinterpret_cast<void***>(sc);
        bool known = false;
        for (const SwapVt& v : g_swapVts) known |= v.vt == vt;
        if (!known)
            for (SwapVt& v : g_swapVts) {
                if (v.vt) continue;
                v.present = reinterpret_cast<SwapPresent_t>(vt[3]);
                v.vt = vt;
                const bool ok = patchSlot(&vt[3], reinterpret_cast<void*>(hkSwapPresent));
                char line[128];
                std::snprintf(line, sizeof line, "hooked swap chain %u vtable %p (%s)", i, static_cast<void*>(vt), ok ? "ok" : "FAILED");
                log(line);
                break;
            }
        sc->Release();
    }
}

// Watches the game's vertex-shader constant uploads for a matrix equal to one of the camera
// copies' view-projection: that copy is exactly what this frame was drawn with.
HRESULT STDMETHODCALLTYPE hkSetVSConstF(IDirect3DDevice9* d, UINT start, const float* data, UINT count) {
    const DeviceVt* v = vtOf(d);
    if (!v) return D3DERR_INVALIDCALL;
    const int nc = g_camCount.load(std::memory_order_relaxed);
    const bool hadVP = g_frameVPValid;
    if (!g_frameVPValid && nc > 0 && data && count >= 4 && !g_inPresent.load(std::memory_order_relaxed)) {
        for (UINT r = 0; r + 4 <= count && !g_frameVPValid; r++) {
            const float* m = data + r * 4;
            for (int c = 0; c < nc; c++) {
                const float* vp = reinterpret_cast<const float*>(g_cams[c] + offsetof(ViewParms, vp));
                bool same = m[0] == vp[0], tr = same;
                for (int i = 1; i < 16 && same; i++) same = m[i] == vp[i];
                for (int i = 1; i < 16 && tr; i++) tr = m[i] == vp[(i & 3) * 4 + (i >> 2)];
                if (same || tr) {
                    std::memcpy(g_frameVP, vp, sizeof g_frameVP);  // stays put until this frame's Present
                    if (tr)
                        for (int i = 0; i < 16; i++) g_frameVP[i] = m[(i & 3) * 4 + (i >> 2)];
                    else
                        std::memcpy(g_frameVP, m, sizeof g_frameVP);
                    std::memcpy(g_frameEye, reinterpret_cast<const void*>(g_cams[c] + kOriginOfs), sizeof g_frameEye);
                    g_frameVPValid = true;
                    IDirect3DSurface9* rt = nullptr;
                    if (SUCCEEDED(d->GetRenderTarget(0, &rt)) && rt) {
                        g_worldRT = rt;
                        rt->Release();
                    }
                    break;
                }
            }
        }
    }
    if (hadVP && !g_drawnThisFrame && data && count >= 4 && !g_inPresent.load(std::memory_order_relaxed)) {
        bool ortho = false;
        for (UINT r = 0; r + 4 <= count && !ortho; r++) ortho = isOrtho2D(data + r * 4);
        if (g_diagLeft > 0) {
            g_diagLeft--;
            char line[400];
            int n = std::snprintf(line, sizeof line, "  vsconst c%u x%u:", start, count);
            for (int i = 0; i < 16 && n > 0 && n < int(sizeof line) - 16; i++) n += std::snprintf(line + n, sizeof line - n, " %.4g", data[i]);
            log(line);
        }
        if (ortho && !g_copyTrigger) {
            IDirect3DSurface9* rt = nullptr;
            if (SUCCEEDED(d->GetRenderTarget(0, &rt)) && rt) {
                renderBeforeHud(d, rt, false);
                rt->Release();
            }
        }
    }
    return v->setVSF(d, start, data, count);
}

// The game's depth buffer: when it makes a plain one the size of the screen, hand it an INTZ
// texture's surface instead, so the fog can read depth. Same format family (24-bit depth +
// stencil), so the game can't tell. Multisampled buffers can't be textures: left alone.
HRESULT STDMETHODCALLTYPE hkCreateDSS(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL discard,
                                      IDirect3DSurface9** out, HANDLE* sh) {
    const DeviceVt* v = vtOf(d);
    if (!v) return D3DERR_INVALIDCALL;
    const D3DFORMAT kINTZ = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
    char line[160];
    if (out && !sh && ms == D3DMULTISAMPLE_NONE && w >= 256 && h >= 256 &&
        (fmt == D3DFMT_D24S8 || fmt == D3DFMT_D24X8 || fmt == D3DFMT_D24FS8 || fmt == D3DFMT_D16 || fmt == D3DFMT_D32)) {
        IDirect3D9* d9 = nullptr;
        D3DDEVICE_CREATION_PARAMETERS cp{};
        D3DDISPLAYMODE dm{};
        bool ok = SUCCEEDED(d->GetDirect3D(&d9)) && d9 && SUCCEEDED(d->GetCreationParameters(&cp)) &&
                  SUCCEEDED(d9->GetAdapterDisplayMode(cp.AdapterOrdinal, &dm)) &&
                  SUCCEEDED(d9->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, dm.Format, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, kINTZ));
        if (d9) d9->Release();
        IDirect3DTexture9* t = nullptr;
        if (ok && SUCCEEDED(d->CreateTexture(w, h, 1, D3DUSAGE_DEPTHSTENCIL, kINTZ, D3DPOOL_DEFAULT, &t, nullptr)) && t &&
            SUCCEEDED(t->GetSurfaceLevel(0, out))) {
            rel(g.depth);
            g.depth = t;  // our reference; the game holds the surface
            std::snprintf(line, sizeof line, "depth: game's %ux%u depth buffer (format %d) is now a readable INTZ texture", w, h,
                          static_cast<int>(fmt));
            log(line);
            return D3D_OK;
        }
        if (t) t->Release();
        std::snprintf(line, sizeof line, "depth: INTZ %s for %ux%u; fog will need floatz", ok ? "creation failed" : "not supported", w, h);
        log(line);
    } else if (ms != D3DMULTISAMPLE_NONE) {
        log("depth: the game's depth buffer is multisampled (anti-aliasing on); fog needs floatz or AA off");
    }
    return v->createDSS(d, w, h, fmt, ms, q, discard, out, sh);
}

// The game copying the finished world out (to a texture its post effects draw back over the screen):
// the effect goes in first.
HRESULT STDMETHODCALLTYPE hkStretchRect(IDirect3DDevice9* d, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr,
                                        D3DTEXTUREFILTERTYPE f) {
    const DeviceVt* v = vtOf(d);
    if (!v) return D3DERR_INVALIDCALL;
    if (src && g_frameVPValid && !g_inPresent.load() && src == g_worldRT) {
        if (!g_copyTrigger) {
            g_copyTrigger = true;
            D3DSURFACE_DESC dd{};
            if (dst) dst->GetDesc(&dd);
            char line[160];
            std::snprintf(line, sizeof line, "hud: the game copies the world target to a %ux%u (format %d) surface", dd.Width, dd.Height,
                          static_cast<int>(dd.Format));
            log(line);
        }
        if (!g_drawnThisFrame) renderBeforeHud(d, src, true);
    }
    return v->stretchRect(d, src, sr, dst, dr, f);
}

HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) {
    const DeviceVt* v = vtOf(d);
    if (!v) return D3DERR_INVALIDCALL;
    if (d == g.dev || !g.dev) {  // default-pool resources must be gone before Reset
        releaseSized();
        rel(g.depth);
    }
    log("device reset");
    return v->reset(d, pp);
}

HRESULT STDMETHODCALLTYPE hkSetRenderTarget(IDirect3DDevice9* d, DWORD idx, IDirect3DSurface9* s) {
    const DeviceVt* v = vtOf(d);
    if (!v) return D3DERR_INVALIDCALL;
    HRESULT hr = v->setRT(d, idx, s);
    static IDirect3DDevice9* swapHooked = nullptr;
    if (swapHooked != d && !g_inPresent.load()) {
        swapHooked = d;
        hookSwapChains(d);
    }
    if (idx == 0 && s && !g_inPresent.load() && (!g.dev || g.dev == d) && !g.floatz) {
        D3DSURFACE_DESC sd;
        if (SUCCEEDED(s->GetDesc(&sd))) {
            if (g.rtLogged < 24) {
                static D3DSURFACE_DESC seen[24];
                bool dup = false;
                for (unsigned i = 0; i < g.rtLogged; i++)
                    dup |= seen[i].Format == sd.Format && seen[i].Width == sd.Width && seen[i].Height == sd.Height;
                if (!dup) {
                    seen[g.rtLogged++] = sd;
                    char line[128];
                    std::snprintf(line, sizeof line, "render target seen: format %d, %ux%u", static_cast<int>(sd.Format), sd.Width, sd.Height);
                    log(line);
                }
            }
            IDirect3DSurface9* bb = nullptr;
            D3DSURFACE_DESC bd{};
            if (sd.Format == D3DFMT_R32F && SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
                bb->GetDesc(&bd);
                bb->Release();
                IDirect3DTexture9* t = nullptr;
                if (sd.Width == bd.Width && sd.Height == bd.Height &&
                    SUCCEEDED(s->GetContainer(IID_IDirect3DTexture9, reinterpret_cast<void**>(&t))) && t) {
                    g.floatz = t;  // GetContainer AddRef'd it
                    log("found the floatz depth target");
                }
            }
        }
    }
    return hr;
}

// Finds the game's device by its vtable entries (pointer in the exe's writable data -> object whose
// vtable holds d3d9's real Present/Reset/SetRenderTarget) and hooks it. Re-runs whenever frames
// stop arriving, which also catches a device the game re-creates (vid_restart).
void findGameDevice() {
    HMODULE exe = GetModuleHandleA(nullptr);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(exe);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    const uintptr_t hi = lo + reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(exe) + dos->e_lfanew)->OptionalHeader.SizeOfImage;
    static std::vector<uintptr_t> buf(1 << 16);
    MEMORY_BASIC_INFORMATION m;
    for (uintptr_t a = lo; a < hi && VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof m) == sizeof m;) {
        const uintptr_t rlo = reinterpret_cast<uintptr_t>(m.BaseAddress), rhi = rlo + m.RegionSize;
        a = rhi;
        const DWORD rw = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (m.State != MEM_COMMIT || !(m.Protect & rw) || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) continue;
        for (uintptr_t c = rlo; c < rhi; c += buf.size() * 8) {
            const size_t n = static_cast<size_t>(std::min<uintptr_t>(buf.size() * 8, rhi - c)) / 8;
            if (!readSelf(c, buf.data(), n * 8)) continue;
            for (size_t i = 0; i < n; i++) {
                const uintptr_t obj = buf[i];
                if (obj < 0x10000 || obj > 0x7FFFFFFFFFFF || (obj & 7)) continue;
                uintptr_t vt = 0;
                void* fns[38];
                if (!readSelf(obj, &vt, 8) || !vt || (vt & 7) || !readSelf(vt, fns, sizeof fns)) continue;
                if (g_realCreateDevice && fns[kCreateDevice] == reinterpret_cast<void*>(g_realCreateDevice)) {
                    hookD3D9Object(reinterpret_cast<IDirect3D9*>(obj));  // the game's IDirect3D9
                    continue;
                }
                if (fns[kPresent] != g_realFns[0] || fns[kReset] != g_realFns[1] || fns[kSetRenderTarget] != g_realFns[2]) continue;
                if (vtOf(reinterpret_cast<IDirect3DDevice9*>(obj))) continue;
                char line[128];
                std::snprintf(line, sizeof line, "game device 0x%llX (pointer at exe+0x%llX)", static_cast<unsigned long long>(obj),
                              static_cast<unsigned long long>(c + i * 8 - lo));
                log(line);
                hookDeviceVtable(reinterpret_cast<IDirect3DDevice9*>(obj));
            }
        }
    }
}

void deviceFinderThread() {
    unsigned last = ~0u;
    int misses = 0;
    for (;;) {
        Sleep(1000);
        const unsigned now = g_presents.load();
        if (now != last) {
            last = now;
            misses = 0;
            continue;
        }
        if (++misses < 2) continue;  // no frame for ~2 s: look for an unhooked device
        findGameDevice();
        misses = 0;
        if (now == 0) Sleep(2000);
    }
}

LRESULT CALLBACK dummyProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

DWORD WINAPI hookThread(LPVOID) {
    HMODULE d3d = LoadLibraryA("d3d9.dll");
    auto create = d3d ? reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(d3d, "Direct3DCreate9")) : nullptr;
    IDirect3D9* d9 = create ? create(D3D_SDK_VERSION) : nullptr;
    if (!d9) {
        log("Direct3DCreate9 failed; no volumetrics");
        return 0;
    }
    // IDirect3D9::CreateDevice: hooks whatever device the game creates from now on.
    void** d9vt = *reinterpret_cast<void***>(d9);
    if (d9vt[kCreateDevice] != reinterpret_cast<void*>(hkCreateDevice)) {
        g_realCreateDevice = reinterpret_cast<CreateDevice_t>(d9vt[kCreateDevice]);
        hookD3D9Vtable(d9vt);
    } else {  // already ours (the game's object shares this class): use its original
        for (const D3DVt& v : g_d3dVts)
            if (v.vt == d9vt) g_realCreateDevice = v.real;
    }
    // A throwaway device: d3d9's device vtable is shared, so this also covers a device the game
    // already made before we were injected.
    WNDCLASSA wc{};
    wc.lpfnWndProc = dummyProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "iw4r_vol_dummy";
    RegisterClassA(&wc);
    HWND hw = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = pp.BackBufferHeight = 8;
    pp.hDeviceWindow = hw;
    // d3d9 uses a different device class per vertex-processing mode, so cover both.
    HMODULE d3dMod = d3d;
    uintptr_t d3dLo = reinterpret_cast<uintptr_t>(d3dMod), d3dHi = d3dLo;
    if (auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(d3dMod))
        d3dHi = d3dLo + reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(d3dMod) + dos->e_lfanew)->OptionalHeader.SizeOfImage;
    for (DWORD vpFlag : {DWORD(D3DCREATE_HARDWARE_VERTEXPROCESSING), DWORD(D3DCREATE_SOFTWARE_VERTEXPROCESSING)}) {
        IDirect3DDevice9* dev = nullptr;
        HRESULT hr = g_realCreateDevice(d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hw, vpFlag | D3DCREATE_DISABLE_DRIVER_MANAGEMENT, &pp, &dev);
        if (SUCCEEDED(hr) && dev) {
            void** vt = *reinterpret_cast<void***>(dev);
            if (!g_realFns[0]) {
                g_realFns[0] = vt[kPresent];
                g_realFns[1] = vt[kReset];
                g_realFns[2] = vt[kSetRenderTarget];
            }
            // A vtable inside d3d9.dll is shared by every device of that class: hooking it covers
            // the game's too. A private heap copy dies with this device, so leave it alone.
            const uintptr_t v = reinterpret_cast<uintptr_t>(vt);
            if (v >= d3dLo && v < d3dHi) hookDeviceVtable(dev);
            dev->Release();
        } else {
            char line[96];
            std::snprintf(line, sizeof line, "dummy device failed (0x%08lX); relying on the CreateDevice hook", hr);
            log(line);
        }
    }
    d9->Release();  // the vtable lives in d3d9.dll, which the game keeps loaded
    if (hw) DestroyWindow(hw);
    std::thread(scannerThread).detach();
    if (g_realFns[0]) std::thread(deviceFinderThread).detach();
    return 0;
}

}  // namespace

void init(LogFn fn) {
    g_log = fn;
    hookCreate9Import();
    CreateThread(nullptr, 0, hookThread, nullptr, 0, nullptr);
}

void onZoneOpen(const std::string& path, const std::string& redirected) {
    const std::string base = lowerBase(path);
    std::lock_guard<std::mutex> l(g_mx);
    if (!redirected.empty()) {
        std::string cfg = redirected.substr(0, redirected.size() - 3) + ".volumetric";
        Settings s;
        if (readSettings(cfg, s)) {
            g_settings = s;
            g_cfgPath = cfg;
            fileTime(cfg, g_cfgTime);
            g_activeMap = base;
            g_camAddr = 0;
            log("map " + base + ": " + (s.enabled ? "on" : "off (enabled 0)") + " from " + cfg);
        }
        return;
    }
    // Another map's zone: stop (stock MP maps, or any SP level's <map>_load.ff).
    if (g_activeMap.empty()) return;
    const bool mpMap = base.rfind("mp_", 0) == 0;
    const bool loadZone = base.size() > 5 && base.compare(base.size() - 5, 5, "_load") == 0;
    if ((mpMap || loadZone) && base != g_activeMap && base != g_activeMap + "_load") {
        log("map " + base + " opened: off");
        g_activeMap.clear();
        g_cfgPath.clear();
        g_settings.enabled = false;
    }
}

}  // namespace volumetric
