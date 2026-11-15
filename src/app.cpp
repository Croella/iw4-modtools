#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include "app.h"
#include <imgui_internal.h>
#include <algorithm>
#include <map>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include "fastfile.h"
#include "mapio.h"
#include "zone/mapimport.h"
#include "zone/zonebuild.h"
#include "light/lightbake.h"
#include "live/liveimport.h"
#include "zone/zonedump.h"
#include "zone/zonesim.h"
#include "model/imageio.h"
#include "model/skeleton.h"
#include "model/xmodelexport.h"
#include <sstream>

namespace {
const char* kToolMaterials[] = {"caulk", "clip", "clip_full", "nodraw", "portal", "trigger", "sky"};

ImU32 rgb(float r, float g, float b, float a = 1) { return ImGui::ColorConvertFloat4ToU32({r, g, b, a}); }

bool containsNoCase(const std::string& text, const char* needle) {
    if (!*needle) return true;
    auto it = std::search(text.begin(), text.end(), needle, needle + std::strlen(needle),
                          [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
    return it != text.end();
}

constexpr char kMapFilter[] = "IW map (*.map)\0*.map\0All files\0*.*\0";
constexpr char kFastfileFilter[] = "Fastfile (*.ff)\0*.ff\0All files\0*.*\0";

std::string fileDialog(HWND owner, bool saving, const std::string& current, const char* filter = kMapFilter,
                       const char* defExt = "map", const char* title = nullptr, const std::string& initialDir = "") {
    char file[MAX_PATH] = "";
    if (!current.empty()) strncpy_s(file, current.c_str(), _TRUNCATE);
    OPENFILENAMEA ofn{};
    if (current.empty() && !initialDir.empty()) ofn.lpstrInitialDir = initialDir.c_str();
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = defExt;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_NOCHANGEDIR | (saving ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    BOOL ok = saving ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn);
    return ok ? file : "";
}

// The directory holding the running editor exe (where iw4r_mapload.dll sits).
std::string exeDir() {
    char buf[MAX_PATH] = "";
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path().string();
}

// Map layout: maps/<map>/<map>.map, with its <map>.gsc, zone_source/ and compiled .ff beside it.
// The maps folder: the current map's (maps/<map>/<map>.map -> maps), else a "maps" folder at or
// above the exe (the repo's maps/ for build/Release), else "" (the dialog's own default).
std::string mapsRoot(const std::string& mapPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!mapPath.empty()) {
        const fs::path dir = fs::path(mapPath).parent_path();
        if (dir.filename() == fs::path(mapPath).stem()) return dir.parent_path().string();
        return dir.string();
    }
    for (fs::path p = exeDir(); !p.empty(); p = p.parent_path()) {
        if (fs::is_directory(p / "maps", ec)) return (p / "maps").string();
        if (p == p.parent_path()) break;
    }
    return "";
}

// A map saved straight into a folder named "maps" goes into its own folder: maps/foo.map ->
// maps/foo/foo.map.
std::string mapFolderPath(const std::string& chosen) {
    namespace fs = std::filesystem;
    const fs::path p(chosen);
    std::string parent = p.parent_path().filename().string();
    for (char& c : parent) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (parent != "maps" || p.stem().empty()) return chosen;
    return (p.parent_path() / p.stem() / p.filename()).string();
}

// Loads dllPath into the target process (CreateRemoteThread -> LoadLibraryA). The editor and the
// game are both x64, so the kernel32 base (hence LoadLibraryA's address) matches across them.
bool injectDll(HANDLE proc, const std::string& dllPath, std::string& err) {
    SIZE_T n = dllPath.size() + 1;
    void* remote = VirtualAllocEx(proc, nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        err = "VirtualAllocEx error " + std::to_string(GetLastError());
        return false;
    }
    bool ok = false;
    if (WriteProcessMemory(proc, remote, dllPath.c_str(), n, nullptr)) {
        auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"));
        if (HANDLE th = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr)) {
            WaitForSingleObject(th, 10000);
            DWORD code = 0;
            GetExitCodeThread(th, &code);  // low 32 bits of the loaded HMODULE; 0 => load failed
            CloseHandle(th);
            if (code != 0) ok = true;
            else err = "LoadLibrary returned null in the target";
        } else {
            err = "CreateRemoteThread error " + std::to_string(GetLastError());
        }
    } else {
        err = "WriteProcessMemory error " + std::to_string(GetLastError());
    }
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return ok;
}

// Pid of a running process named exeName other than `skip` (0 if none).
DWORD findProcess(const std::string& exeName, DWORD skip) {
    DWORD found = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    const std::wstring want(exeName.begin(), exeName.end());
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        if (pe.th32ProcessID != skip && _wcsicmp(pe.szExeFile, want.c_str()) == 0) found = pe.th32ProcessID;
    CloseHandle(snap);
    return found;
}

// Classic folder picker (no COM init needed); returns "" if cancelled.
std::string folderDialog(HWND owner, const char* title) {
    char path[MAX_PATH] = "";
    BROWSEINFOA bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return "";
    bool ok = SHGetPathFromIDListA(pidl, path);
    CoTaskMemFree(pidl);
    return ok ? path : "";
}
}  // namespace

void App::init(ID3D11Device* device, ID3D11DeviceContext* context, const std::string& gamePath) {
    std::string error;
    if (!renderer.init(device, context, error)) log("Renderer error: " + error);
    renderer.setAssets(&assets);
    renderer.setModels(&liveModels);
    gameInput = gamePath;
    char base[MAX_PATH] = ".";
    if (DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH); n == 0 || n >= MAX_PATH) strcpy_s(base, ".");
    cacheDir = std::string(base) + "\\iw4-radiant";
    loadLaunchConfig();
    assets.init(gameInput, cacheDir, [this](const std::string& s) { log(s); });
    log("Ready. Drag in a grid view to create a brush.");
}

void App::shutdown() {
    if (bakeThread.joinable()) {
        bakeProgress.cancel = true;
        bakeThread.join();
    }
    assets.shutdown();
}

void App::startBake() {
    if (bakeThread.joinable()) return;
    // Bake a snapshot so editing can continue meanwhile.
    auto snapshot = std::make_shared<Document>();
    snapshot->ents = doc.ents;
    snapshot->revision = doc.revision;
    bakeProgress.fraction = 0;
    bakeProgress.cancel = false;
    bakeFinished = false;
    bakeOut = std::make_shared<light::Bake>();
    bakeStarted = std::chrono::steady_clock::now();
    refreshUnlitMaterials();
    log("Baking lighting ...");
    bakeThread = std::thread([this, snapshot, s = lightSettings, out = bakeOut] {
        std::string err;
        bakeOk = light::bake(*snapshot, s, *out, err, &bakeProgress);
        bakeError = err;
        bakeFinished = true;
    });
}

void App::pollBake() {
    if (!bakeFinished) return;
    bakeThread.join();
    bakeFinished = false;
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - bakeStarted).count();
    if (!bakeOk) {
        log(bakeProgress.cancel ? "Lighting bake cancelled." : "Lighting bake failed: " + bakeError);
        return;
    }
    bakeResult = bakeOut;
    renderer.setLighting(bakeResult);
    meshRevision = 0;
    char line[200];
    std::snprintf(line, sizeof line, "Baked lighting in %.1fs: %zu faces, %dx%d luxel atlas at %.0f units/luxel%s", secs,
                  bakeResult->faces.size(), bakeResult->width, bakeResult->height, bakeResult->luxelSize,
                  bakeResult->env.hasSun ? "" : " (no sun)");
    log(line);
    startFxPreview("after the bake");
}

// Effect materials (additive falloff sheets) in the map: the baker lets light through them.
void App::refreshUnlitMaterials() {
    lightSettings.unlitMaterials.clear();
    for (const Brush& b : doc.ents.empty() ? std::vector<Brush>{} : doc.ents[0].brushes)
        for (const Face& f : b.faces) {
            matdef::Def def;
            if (assets.materialDef(f.material, def) && matdef::shadingFor(def) == matdef::Shading::FalloffAdd) {
                std::string n = matdef::key(f.material);
                lightSettings.unlitMaterials.insert(n.rfind("wc/", 0) == 0 ? n.substr(3) : n);
            }
        }
}

// File > Edit Map Script: the map's own GSC lives next to the .map as <name>.gsc and replaces
// the generated maps/[mp/]<name>.gsc when compiling. Creates it from the generated script (for
// the last compile target) if missing, then opens it in the system's editor for .gsc (Notepad if
// none is associated).
void App::editMapScript() {
    if (doc.path.empty()) {
        save(true);
        if (doc.path.empty()) return;
    }
    const std::string path = mapScriptPath(doc.path);
    if (!std::filesystem::exists(path)) {
        const std::string name = std::filesystem::path(doc.path).stem().string();
        std::ofstream f(path, std::ios::binary);
        std::string src = mapScriptSource(name, compileTarget);
        if (compileTarget == MapTarget::SP)
            src += "\n// Add your level logic below _load::main(), e.g.\n"
                   "// level.player thread my_intro();\n";
        else
            src += "\n// Add your level logic after maps\\mp\\_load::main(), e.g. thread functions.\n";
        f << src;
        if (!f) {
            MessageBoxA(hwnd, ("Couldn't create " + path).c_str(), "Edit Map Script", MB_OK | MB_ICONERROR);
            return;
        }
        log("Created map script " + path + " (" + (compileTarget == MapTarget::SP ? "singleplayer" : "multiplayer") +
            " template)");
    }
    if (reinterpret_cast<INT_PTR>(ShellExecuteA(hwnd, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        ShellExecuteA(hwnd, "open", "notepad.exe", ("\"" + path + "\"").c_str(), nullptr, SW_SHOWNORMAL);
    log("Editing " + path + ": it's packed into the map as its script on the next compile (F5/F7).");
}

// Weapon pack (worldspawn "weaponpack", SP): copies the stock add-on zone next to the map's zone
// as so_w_<map>.ff. Returns the level name to devmap, or "" if the map has no pack.
std::string App::deployPackFor(const std::string& mapName, MapTarget target, const std::string& folder) {
    const std::string* pack = doc.ents.empty() ? nullptr : doc.ents[0].find("weaponpack");
    if (!pack || pack->empty() || target != MapTarget::SP) return "";
    std::string err;
    if (!deployWeaponPack(gameInput, folder, *pack, mapName, err)) {
        log("  weapon pack: " + err);
        return "";
    }
    const std::string level = weaponPackLevel(mapName);
    log("  weapon pack " + *pack + " deployed as " + level + ".ff: play with  devmap " + level);
    return level;
}

void App::drawWeaponsMenu() {
    if (!ImGui::BeginMenu("Weapons")) return;
    const std::string* cur = doc.ents.empty() ? nullptr : doc.ents[0].find("weaponpack");
    const std::string current = cur ? *cur : "";
    ImGui::TextDisabled("Singleplayer: guns come from a stock Special Ops add-on zone");
    ImGui::TextDisabled("loaded with the map. Play with  devmap so_w_<map>.");
    ImGui::Separator();
    auto setPack = [&](const std::string& v) {
        doc.pushUndo();
        if (v.empty()) {
            auto& kv = doc.ents[0].kv;
            kv.erase(std::remove_if(kv.begin(), kv.end(), [](const KeyValue& k) { return k.key == "weaponpack"; }), kv.end());
        } else {
            doc.ents[0].set("weaponpack", v);
        }
        doc.touch();
        log(v.empty() ? "Weapon pack removed" : "Weapon pack: " + v + " (compile for Singleplayer, then devmap " +
                                                    weaponPackLevel(doc.path.empty() ? "<map>" : std::filesystem::path(doc.path).stem().string()) + ")");
    };
    if (ImGui::MenuItem("None (grenades, c4 only)", nullptr, current.empty())) setPack("");
    for (const WeaponPack& p : weaponPacks()) {
        if (ImGui::MenuItem(p.zone, nullptr, current == p.zone)) setPack(p.zone);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.weapons);
    }
    ImGui::EndMenu();
}

void App::bakeLightingNow() {
    startBake();
    waitForBake();
}

void App::waitForBake() {
    if (!bakeThread.joinable()) return;
    log("Waiting for the lighting bake ...");
    while (!bakeFinished) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    pollBake();
}

uint64_t App::currentSceneHash() {
    if (hashRevision != doc.revision) {
        hashValue = light::sceneHash(doc);
        hashRevision = doc.revision;
    }
    return hashValue;
}

bool App::bakeUpToDate() { return bakeResult && bakeResult->sceneHash == currentSceneHash(); }

void App::addLightAtCamera() {
    doc.pushUndo();
    Vec3 p = camera.pos + camera.forward() * 96.0;
    Sel s = doc.addPointEntity("light", {snapTo(p.x, gridSize), snapTo(p.y, gridSize), snapTo(p.z, gridSize)});
    doc.ents[s.ent].set("radius", "300");
    doc.ents[s.ent].set("intensity", "1");
    doc.ents[s.ent].set("_color", "1 1 1");
    doc.sel = {s};
    doc.touch();
    log("Added a light (radius 300). Edit radius / intensity / _color in the Inspector, then F9 to bake.");
}

ImVec2 App::gridToScreen(int index, double u, double v) const {
    const GridView& view = views[index];
    const ViewRect& r = gridRects[index];
    return {r.pos.x + r.size.x * 0.5f + static_cast<float>((u - view.centerU) * view.zoom),
            r.pos.y + r.size.y * 0.5f - static_cast<float>((v - view.centerV) * view.zoom)};
}

void App::log(const std::string& text) {
    std::lock_guard lock(logMutex);  // the asset thread logs too
    logLines.push_back(text);
    if (logLines.size() > 500) logLines.pop_front();
    logScroll = true;
}

bool App::confirmDiscard() {
    if (!doc.dirty) return true;
    int r = MessageBoxA(hwnd, "Save changes to the current map?", "iw4-radiant", MB_YESNOCANCEL | MB_ICONWARNING);
    if (r == IDCANCEL) return false;
    if (r == IDYES) {
        save(false);
        return !doc.dirty;  // a failed or cancelled save keeps the user in the editor
    }
    return true;
}

void App::newMap() {
    if (!confirmDiscard()) return;
    doc.reset();
    log("New map");
}

// zone_source models (zone_source/xmodels/*.xmodel_export next to the map) into the model preview
// library, so script_model / misc_model entities using them draw in the 3D view.
void App::loadZoneSourcePreview() {
    if (doc.path.empty()) return;
    const std::string dir = zoneSourcePath(doc.path);
    if (!std::filesystem::is_directory(std::filesystem::path(dir) / "xmodels")) return;
    std::vector<std::string> warnings;
    // Same build options as the compiler (weapon hideTags drop attachment geometry, ...), so the
    // preview and the Models browser show what the game will.
    std::vector<std::string> weaponWarnings;
    ZoneSourceWeapons zw = loadZoneSourceWeapons(dir, weaponWarnings);
    std::vector<modelbuild::Model> models = loadZoneSourceModels(dir, warnings, &zw.hints);
    for (const std::string& w : warnings) log("  zone_source: " + w);
    browserModels.clear();
    renderer.clearThumbnails();
    for (const modelbuild::Model& md : models) {
        auto lm = std::make_shared<live::LiveModel>();
        lm->name = md.name;
        BrowserModel bm;
        bm.name = md.name;
        bm.bones = md.bones.size();
        bm.surfaces = md.surfaces.size();
        for (const modelbuild::Surface& s : md.surfaces) {
            bm.tris += s.tris.size() / 3;
            bm.verts += s.verts.size();
            const std::string& tex = md.materials[s.material].texture;
            const std::string t = tex.empty() ? md.materials[s.material].name : std::filesystem::path(tex).filename().string();
            if (std::find(bm.textures.begin(), bm.textures.end(), t) == bm.textures.end()) bm.textures.push_back(t);
        }
        bm.size = Vec3{md.maxs[0] - md.mins[0], md.maxs[1] - md.mins[1], md.maxs[2] - md.mins[2]};
        browserModels.push_back(std::move(bm));
        for (const modelbuild::Surface& s : md.surfaces) {
            live::LiveModelSurf ls;
            const modelbuild::Material& mm = md.materials[s.material];
            ls.material = "mc/" + md.name + "_" + mm.name;
            // Same search as the compiler (planModelMaterials): zone_source/images, then next to the
            // models in zone_source/xmodels; exported name, stem.png/.tga, and without t5_/t6_/t7_.
            std::string stem = std::filesystem::path(mm.texture).stem().string();
            if (stem.empty()) stem = mm.name;
            std::string bare = stem;
            for (const char* pre : {"t5_", "t6_", "t7_", "T5_", "T6_", "T7_"})
                if (bare.rfind(pre, 0) == 0) bare = bare.substr(3);
            bool found = false;
            for (const char* sub : {"images", "xmodels"}) {
                std::vector<std::string> cands;
                if (!mm.texture.empty()) cands.push_back(std::filesystem::path(mm.texture).filename().string());
                for (const std::string& st : {stem, bare})
                    for (const char* ext : {".png", ".tga", ".PNG", ".TGA"}) cands.push_back(st + ext);
                for (const std::string& cand : cands) {
                    std::filesystem::path p = std::filesystem::path(dir) / sub / cand;
                    std::error_code ec;
                    if (!cand.empty() && std::filesystem::is_regular_file(p, ec)) {
                        renderer.setTextureFile(ls.material, p.string());
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }
            for (const modelbuild::PackedVertex& v : s.verts) {
                ls.pos.insert(ls.pos.end(), v.xyz, v.xyz + 3);
                Vec3 n = gfx::unpackUnitVec(v.normal);
                ls.nrm.push_back(static_cast<float>(n.x));
                ls.nrm.push_back(static_cast<float>(n.y));
                ls.nrm.push_back(static_cast<float>(n.z));
                auto half = [](uint16_t h) {
                    uint32_t sign = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF, bits;
                    if (e == 0) bits = sign << 31;
                    else bits = (sign << 31) | ((e - 15 + 127) << 23) | (m << 13);
                    float f;
                    std::memcpy(&f, &bits, 4);
                    return f;
                };
                ls.uv.push_back(half(static_cast<uint16_t>(v.texCoord & 0xFFFF)));
                ls.uv.push_back(half(static_cast<uint16_t>(v.texCoord >> 16)));
                lm->bounds.add(Vec3{v.xyz[0], v.xyz[1], v.xyz[2]});
            }
            // The game's winding is clockwise from the front; the preview draws both sides anyway.
            ls.idx = s.tris;
            lm->surfs.push_back(std::move(ls));
        }
        liveModels[live::modelKey(md.name)] = lm;
    }
    meshRevision = 0;
    if (!models.empty()) log("Loaded " + std::to_string(models.size()) + " zone_source model(s) for preview from " + dir);
}

bool App::openMap(const std::string& path) {
    std::string err;
    if (!loadMap(path, doc, err)) {
        log("Open failed: " + err);
        MessageBoxA(hwnd, err.c_str(), "Couldn't open map", MB_OK | MB_ICONERROR);
        return false;
    }
    size_t brushes = 0, raw = 0;
    for (const Entity& e : doc.ents) {
        brushes += e.brushes.size();
        raw += e.rawBlocks.size();
    }
    log("Opened " + path + ": " + std::to_string(doc.ents.size()) + " entities, " + std::to_string(brushes) + " brushes" +
        (raw ? ", " + std::to_string(raw) + " raw blocks kept but not shown" : ""));
    loadZoneSourcePreview();
    return true;
}

void App::openDialog() {
    if (!confirmDiscard()) return;
    std::string p = fileDialog(hwnd, false, doc.path, kMapFilter, "map", nullptr, mapsRoot(""));
    if (!p.empty()) openMap(p);
}

void App::save(bool forceDialog) {
    namespace fs = std::filesystem;
    std::string path = doc.path;
    if (forceDialog || path.empty()) {
        path = fileDialog(hwnd, true, path, kMapFilter, "map", nullptr, mapsRoot(doc.path));
        if (path.empty()) return;
        path = mapFolderPath(path);  // maps/foo.map -> maps/foo/foo.map
    }
    if (path.empty()) return;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    // Save As into a new folder: the map's custom assets and script come along.
    if (!doc.path.empty() && fs::path(doc.path).parent_path() != fs::path(path).parent_path()) {
        const fs::path oldZs = zoneSourcePath(doc.path), newZs = zoneSourcePath(path);
        if (fs::is_directory(oldZs, ec) && !fs::exists(newZs, ec)) {
            fs::copy(oldZs, newZs, fs::copy_options::recursive, ec);
            log(ec ? "  couldn't copy zone_source: " + ec.message() : "  copied zone_source to " + newZs.string());
        }
        const fs::path oldGsc = mapScriptPath(doc.path), newGsc = mapScriptPath(path);
        if (fs::exists(oldGsc, ec) && !fs::exists(newGsc, ec) && fs::copy_file(oldGsc, newGsc, ec))
            log("  copied the map script to " + newGsc.string());
    }
    std::string err;
    if (saveMap(path, doc, err)) {
        doc.path = path;
        doc.dirty = false;
        log("Saved " + path);
    } else {
        log("Save failed: " + err);
        MessageBoxA(hwnd, err.c_str(), "Couldn't save map", MB_OK | MB_ICONERROR);
    }
}

void App::requestCompile(PendingCompile what) { pendingCompile = what; }

// Modal asking which game the compile targets. Opened by requestCompile; runs the compile on a choice.
void App::drawCompileTargetPopup() {
    const char* id = "Compile for";
    if (pendingCompile != PendingCompile::None && !ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted(pendingCompile == PendingCompile::AndRun ? "Compile & run this map in:"
                                                                     : "Compile this map for:");
    ImGui::Spacing();
    int pick = -1;
    const ImVec2 size(220, 0);
    auto button = [&](const char* label, MapTarget t) {
        bool isDefault = compileTarget == t;
        if (isDefault) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(label, size)) pick = static_cast<int>(t);
        if (isDefault) ImGui::PopStyleColor();
    };
    button("Multiplayer (iw4mp)", MapTarget::MP);
    ImGui::SameLine();
    button("Singleplayer (iw4sp)", MapTarget::SP);
    ImGui::TextDisabled("Enter = last choice, Esc = cancel");
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
        pick = static_cast<int>(compileTarget);
    bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::Button("Cancel");
    if (pick < 0 && !cancel) {
        ImGui::EndPopup();
        return;
    }
    PendingCompile what = pendingCompile;
    pendingCompile = PendingCompile::None;
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    if (pick < 0) return;
    compileTarget = static_cast<MapTarget>(pick);
    if (what == PendingCompile::ToFile) compileDialog(compileTarget);
    else compileAndRun(compileTarget);
}

// MP maps are mp_<name> (the game loads maps/mp/mp_<name>.d3dbsp); SP maps must not be, since the
// SP game's own zones are plain <name>.ff. Returns false if the user backs out.
static bool confirmMapName(HWND hwnd, const std::string& mapName, MapTarget target, const char* title) {
    const bool mpName = mapName.rfind("mp_", 0) == 0;
    if (target == MapTarget::MP && !mpName)
        return MessageBoxA(hwnd, ("\"" + mapName + "\" doesn't start with mp_. Multiplayer maps are normally named mp_<name>.\n\n"
                                  "Continue anyway?").c_str(),
                           title, MB_YESNO | MB_ICONQUESTION) == IDYES;
    if (target == MapTarget::SP && mpName)
        return MessageBoxA(hwnd, ("\"" + mapName + "\" starts with mp_, but you're compiling for singleplayer.\n\n"
                                  "Continue anyway?").c_str(),
                           title, MB_YESNO | MB_ICONQUESTION) == IDYES;
    return true;
}

// Copies a compiled zone (and its sun-shaft settings) into <game>/zone/english, where the game
// finds it on its own. Skipped when `already` is that same file.
void App::deployToGameZone(const std::string& mapName, const std::vector<uint8_t>& ff, const std::string& already) {
    namespace fs = std::filesystem;
    if (gameInput.empty()) return;
    const fs::path dir = fs::path(gameInput) / "zone" / "english";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        log("  zone/english not found in the game folder; not copied there (" + dir.string() + ")");
        return;
    }
    const fs::path dest = dir / (mapName + ".ff");
    if (!already.empty() && fs::equivalent(dest, already, ec)) return;
    std::ofstream f(dest, std::ios::binary | std::ios::trunc);
    if (!f.write(reinterpret_cast<const char*>(ff.data()), static_cast<std::streamsize>(ff.size()))) {
        log("  can't write " + dest.string() + " (is the game running with the map loaded?)");
        return;
    }
    f.close();
    std::ofstream(dir / (mapName + ".volumetric"), std::ios::trunc) << light::volumetricConfig(doc);
    log("  copied to " + dest.string());
}

void App::compileDialog(MapTarget target) {
    // Default: <map dir>/<map stem>.ff, or the last compile target.
    std::string initial = lastCompilePath;
    if (initial.empty() && !doc.path.empty())
        initial = (std::filesystem::path(doc.path).replace_extension(".ff")).string();
    std::string path = fileDialog(hwnd, true, initial, kFastfileFilter, "ff", "Compile map to fastfile");
    if (path.empty()) return;
    lastCompilePath = path;

    // The file name is the map name the game loads: mp_foo.ff -> maps/mp/mp_foo.d3dbsp (MP),
    // foo.ff -> maps/foo.d3dbsp (SP); "devmap <name>" in either game.
    const std::string mapName = std::filesystem::path(path).stem().string();
    if (!confirmMapName(hwnd, mapName, target, "Compile to .ff")) return;

    std::string err;
    std::vector<uint8_t> ff;
    if (!compileCurrentMap(mapName, target, ff, err)) {
        log("Compile failed: " + err);
        MessageBoxA(hwnd, err.c_str(), "Compile failed", MB_OK | MB_ICONERROR);
        return;
    }
    zone::SimulatedZone sim;
    Fastfile back;
    readFastfile(ff, back, err);
    zone::simulateLoad(back.zone, sim, err);  // for the details string; compileCurrentMap already verified it
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.write(reinterpret_cast<const char*>(ff.data()), static_cast<std::streamsize>(ff.size()))) {
        log("Compile failed: can't write " + path);
        MessageBoxA(hwnd, ("Can't write " + path).c_str(), "Compile failed", MB_OK | MB_ICONERROR);
        return;
    }
    f.close();
    std::string details = zone::describeSimulatedZone(sim);
    const char* gameName = target == MapTarget::SP ? "singleplayer (iw4sp)" : "multiplayer (iw4mp)";
    log("Compiled " + path + " for " + gameName + " (" + std::to_string(ff.size()) + " bytes), loader check ok");
    deployToGameZone(mapName, ff, path);
    std::string packNote = deployPackFor(mapName, target, std::filesystem::path(path).parent_path().string());
    log(details);
    MessageBoxA(hwnd,
                ("Wrote " + path + " for " + gameName + "\n\nLoader check passed.\n" + details +
                 "\nLoad it in-game with:  devmap " + (packNote.empty() ? mapName : packNote) +
                 "\n\nNot compiled yet: brush entities, patches, models, real lighting.")
                    .c_str(),
                "Compile to .ff", MB_OK | MB_ICONINFORMATION);
}

// Compiles the current document to an unsigned fastfile and verifies the game's loader accepts it.
bool App::compileCurrentMap(const std::string& mapName, MapTarget target, std::vector<uint8_t>& ff,
                            std::string& err) {
    Fastfile back;
    zone::SimulatedZone sim;
    waitForBake();
    CompileOptions options;
    options.light = lightSettings;
    refreshUnlitMaterials();
    if (!bakeUpToDate()) {
        log("Baking lighting for the compile ...");
        auto fresh = std::make_shared<light::Bake>();
        auto t0 = std::chrono::steady_clock::now();
        if (!light::bake(doc, lightSettings, *fresh, err)) {
            err = "lighting: " + err;
            return false;
        }
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        bakeResult = fresh;
        renderer.setLighting(bakeResult);
        meshRevision = 0;
        log("  baked in " + std::to_string(static_cast<int>(secs + 0.5)) + "s");
    } else {
        log("Using the current lighting bake.");
    }
    options.prebaked = bakeResult.get();
    options.gameDir = gameInput;
    options.cacheDir = cacheDir;
    if (const std::string* pack = doc.ents.empty() ? nullptr : doc.ents[0].find("weaponpack");
        pack && !pack->empty()) {
        if (target == MapTarget::SP) {
            options.addonLevel = weaponPackLevel(mapName);
            log("Weapon pack " + *pack + ": the map also builds level " + options.addonLevel);
        } else {
            log("  note: weaponpack is singleplayer-only (Special Ops add-on zones); ignored for MP");
        }
    }
    if (!doc.path.empty()) {
        options.zoneSourceDir = zoneSourcePath(doc.path);
        if (std::filesystem::is_directory(options.zoneSourceDir)) log("Compiling custom assets from " + options.zoneSourceDir);
    }
    if (readMapScript(doc.path, options.mapScript))
        log("Using map script " + mapScriptPath(doc.path));
    else
        log("No " + std::filesystem::path(mapScriptPath(doc.path.empty() ? "map.map" : doc.path)).filename().string() +
            " next to the map: using the generated script (File > Edit Map Script to customise)");
    std::vector<std::string> warnings;
    options.warnings = &warnings;
    bool compiled = compileMapToFastfile(doc, mapName, ff, err, target, options);
    for (const std::string& w : warnings) log("  warning: " + w);
    if (!compiled || !readFastfile(ff, back, err)) return false;
    if (!zone::simulateLoad(back.zone, sim, err)) {  // never ship a zone the loader would reject
        err = "loader check failed: " + err;
        return false;
    }
    startFxPreview("after the compile");
    return true;
}

// Loads zone_source/fx and the effect materials (textures handed to the renderer), places an emitter
// on every fx_origin, and pre-runs a few seconds so the preview starts populated.
void App::startFxPreview(const char* why) {
    if (doc.path.empty()) return;
    const std::string zs = zoneSourcePath(doc.path);
    bool anyFx = false;
    for (const Entity& e : doc.ents) anyFx |= e.classname() == "fx_origin";
    if (!anyFx && !showEffects) return;
    FxPreviewMaterials mats(zs, gameInput, cacheDir);
    auto lookup = [&](const std::string& name) {
        fxpreview::MaterialInfo mi;
        const FxPreviewMaterial& m = mats.get(name);
        mi.found = m.found;
        mi.additive = m.additive;
        mi.rows = std::max(1, m.rows);
        mi.cols = std::max(1, m.cols);
        if (m.found && !m.pixels.empty() && m.width > 0 && m.height > 0) {
            IwiImage img;
            IwiLevel lvl;
            lvl.width = m.width;
            lvl.height = m.height;
            if (m.format == 0x31545844u) img.format = IwiFormat::Dxt1, lvl.rowPitch = std::max(1, (m.width + 3) / 4) * 8;
            else if (m.format == 0x33545844u) img.format = IwiFormat::Dxt3, lvl.rowPitch = std::max(1, (m.width + 3) / 4) * 16;
            else if (m.format == 0x35545844u) img.format = IwiFormat::Dxt5, lvl.rowPitch = std::max(1, (m.width + 3) / 4) * 16;
            else img.format = IwiFormat::Rgba, lvl.rowPitch = m.width * 4;
            lvl.data = m.pixels;
            img.levels.push_back(std::move(lvl));
            renderer.setTextureImage(name, img);
        }
        return mi;
    };
    std::vector<std::string> warnings;
    fxPreview.load(zs, lookup, warnings);
    for (const std::string& w : warnings) log("  " + w);
    fxPreview.sync(doc);
    fxRevision = doc.revision;
    fxPreview.warmup(8.0);  // long enough for most effects to reach a steady state
    showEffects = true;
    std::string line = "Effects preview " + std::string(why) + ": " + std::to_string(fxPreview.emitterCount()) + " fx_origin(s) playing, " +
                       std::to_string(fxPreview.effectCount()) + " effect(s) in zone_source/fx (View > Effects Preview)";
    log(line);
    for (const std::string& m : fxPreview.missing()) log("  fx_origin effect not in zone_source/fx: " + m);
}

void App::updateFxPreview() {
    if (!showEffects || !fxPreview.loaded()) {
        renderer.setSprites({});
        return;
    }
    if (fxRevision != doc.revision) {
        fxPreview.sync(doc);  // moved / added / removed fx_origins
        fxRevision = doc.revision;
    }
    fxPreview.setViewer(camera.pos);  // the camera walks into trigger_radius volumes like a player
    fxPreview.update(ImGui::GetIO().DeltaTime);
    std::vector<fxpreview::Sprite> sprites;
    fxPreview.sprites(camera.pos, camera.forward(), camera.right(), camera.up(), sprites);
    renderer.setSprites(std::move(sprites));
}

void App::loadLaunchConfig() {
    std::ifstream f(cacheDir + "\\launch.cfg");
    if (f) std::getline(f, deployDir);
}
void App::saveLaunchConfig() {
    std::ofstream f(cacheDir + "\\launch.cfg", std::ios::trunc);
    if (f) f << deployDir;
}

// File > Compile & Run in Game: save the .map, compile it, copy <name>.ff into the game's
// custom-zone folder, launch iw4mp.exe or iw4sp.exe, and inject the zone-redirect DLL.
void App::compileAndRun(MapTarget target) {
    // Need a saved .map so the map has a name; save first (dialog if it has no path yet).
    if (doc.path.empty()) {
        save(true);
        if (doc.path.empty()) return;  // cancelled
    } else if (doc.dirty) {
        save(false);
        if (doc.dirty) return;  // save failed
    }
    const std::string mapName = std::filesystem::path(doc.path).stem().string();
    if (!confirmMapName(hwnd, mapName, target, "Compile & Run")) return;
    const std::string exeName = target == MapTarget::SP ? "iw4sp.exe" : "iw4mp.exe";

    // Where the game loads custom zones from (asked once, then remembered).
    if (deployDir.empty() || !std::filesystem::is_directory(deployDir)) {
        std::string picked = folderDialog(hwnd, "Choose the folder the game loads custom zones from");
        if (picked.empty()) return;
        deployDir = picked;
        saveLaunchConfig();
    }

    std::vector<uint8_t> ff;
    std::string err;
    if (!compileCurrentMap(mapName, target, ff, err)) {
        log("Compile failed: " + err);
        MessageBoxA(hwnd, err.c_str(), "Compile & Run", MB_OK | MB_ICONERROR);
        return;
    }
    std::string dest = (std::filesystem::path(deployDir) / (mapName + ".ff")).string();
    {
        std::ofstream f(dest, std::ios::binary | std::ios::trunc);
        if (!f.write(reinterpret_cast<const char*>(ff.data()), static_cast<std::streamsize>(ff.size()))) {
            log("Compile & Run: can't write " + dest);
            MessageBoxA(hwnd, ("Can't write the zone to:\n" + dest).c_str(), "Compile & Run", MB_OK | MB_ICONERROR);
            return;
        }
    }
    log("Deployed " + dest + " (" + std::to_string(ff.size()) + " bytes)");
    {  // sun-shaft settings for iw4r_mapload.dll (re-read live by the game while it runs)
        const std::string vdest = (std::filesystem::path(deployDir) / (mapName + ".volumetric")).string();
        std::ofstream(vdest, std::ios::trunc) << light::volumetricConfig(doc);
    }
    deployToGameZone(mapName, ff, dest);
    const std::string packLevel = deployPackFor(mapName, target, deployDir);

    std::string exe = (std::filesystem::path(gameInput) / exeName).string();
    if (!std::filesystem::exists(exe)) {
        std::string m = exeName + " isn't in the game folder:\n" + gameInput +
                        "\n\nSet the Game folder in Settings, then try again.";
        log("Compile & Run: " + m);
        MessageBoxA(hwnd, m.c_str(), "Compile & Run", MB_OK | MB_ICONERROR);
        return;
    }
    // Launch the game with no command-line options; the user runs "devmap <map>" from the console
    // after the loader DLL is injected, so the zone load can't race the hooks.
    std::string cmd = "\"" + exe + "\"";
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(exe.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, gameInput.c_str(), &si, &pi)) {
        DWORD e = GetLastError();
        log("Compile & Run: launch failed (CreateProcess error " + std::to_string(e) + ")");
        MessageBoxA(hwnd, ("Couldn't launch:\n" + exe + "\n\nError " + std::to_string(e)).c_str(), "Compile & Run",
                    MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    log("Launched " + exeName);
    // Inject the loader DLL so the game finds the deployed zone. It sits next to the editor exe.
    std::string dll = (std::filesystem::path(exeDir()) / "iw4r_mapload.dll").string();
    std::string injErr;
    if (!std::filesystem::exists(dll))
        log("  note: iw4r_mapload.dll not found next to the editor; map won't load without it (" + dll + ")");
    else if (injectDll(pi.hProcess, dll, injErr))
        log("  injected iw4r_mapload.dll (redirects " + mapName + ".ff from " + deployDir + ")");
    else
        log("  DLL injection failed: " + injErr);
    // Steam often closes the process we started and relaunches the game itself, which would leave
    // the real game without the DLL. Follow the relaunch and inject into the new process too.
    if (std::filesystem::exists(dll)) {
        std::thread([this, proc = pi.hProcess, pid = pi.dwProcessId, exeName, dll] {
            if (WaitForSingleObject(proc, 20000) == WAIT_TIMEOUT) {  // still running: it's the real game
                CloseHandle(proc);
                return;
            }
            CloseHandle(proc);
            for (int i = 0; i < 3000; i++) {  // up to 60 s for Steam to start the new one
                Sleep(20);
                const DWORD np = findProcess(exeName, pid);
                if (!np) continue;
                // Inject right away: the DLL has to be in before the game creates its Direct3D device
                // (its depth buffer is made then). The remote LoadLibrary waits for the process's
                // own startup to finish by itself.
                Sleep(30);
                HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                           PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, np);
                std::string e;
                if (!h) {
                    log("  Steam relaunched " + exeName + " but it can't be opened (error " + std::to_string(GetLastError()) + ")");
                } else if (injectDll(h, dll, e)) {
                    log("  Steam relaunched " + exeName + "; injected iw4r_mapload.dll into it (pid " + std::to_string(np) + ")");
                } else {
                    log("  Steam relaunched " + exeName + "; DLL injection failed: " + e);
                }
                if (h) CloseHandle(h);
                return;
            }
            log("  " + exeName + " exited and no relaunch was seen; the DLL isn't in the game");
        }).detach();
    } else {
        CloseHandle(pi.hProcess);
    }
    log("  When the game is up, open the console (~) and run:  devmap " + (packLevel.empty() ? mapName : packLevel));
}

bool App::importFastfileMap(const std::string& path) {
    Document imported;
    mapimport::ImportStats st;
    std::string err;
    log("Importing " + path + " ...");
    if (!mapimport::importFastfile(path, imported, st, err)) {
        log("Import failed: " + err);
        MessageBoxA(hwnd, err.c_str(), "Import map", MB_OK | MB_ICONERROR);
        return false;
    }
    doc = std::move(imported);
    meshRevision = 0;  // force a renderer rebuild
    // Stock collision is mostly caulk/clip hulls that would hide the render mesh in 3D.
    if (doc.refMesh) renderer.showToolFaces = false;
    if (doc.refMesh && !doc.refMesh->materialImages.empty()) {
        assets.addMaterialHints(doc.refMesh->materialImages);
        renderer.clearTextures();  // re-resolve anything already shown as a checker
    }
    frameAll();
    // Stock maps are wrapped in a huge sky/caulk shell, so start at the player start instead.
    for (const Entity& e : doc.ents) {
        std::string cls = e.classname();
        Vec3 o;
        if ((cls == "info_player_start" || cls == "mp_global_intermission" || cls.rfind("mp_", 0) == 0) && e.origin(o)) {
            camera.pos = o + Vec3{0, 0, 60};
            double pitch = 0, yaw = 0, roll = 0;
            if (const std::string* a = e.find("angles"))
                std::sscanf(a->c_str(), "%lf %lf %lf", &pitch, &yaw, &roll);
            camera.yaw = yaw;
            camera.pitch = -10;
            camSpeed = 600;
            for (GridView& v : views) {
                v.centerU = o[v.u];
                v.centerV = o[v.v];
                v.zoom = 0.12;
            }
            break;
        }
    }
    std::string summary = std::to_string(st.entities) + " entities, " + std::to_string(st.brushes) + " brushes" +
                          (st.brushesSkipped ? " (" + std::to_string(st.brushesSkipped) + " skipped)" : "") + ", " +
                          std::to_string(st.surfaces) + " render surfaces / " + std::to_string(st.triangles) +
                          " triangles (" + std::to_string(st.surfacesTextured) + " with a material)";
    log("Imported " + path + ": " + summary);
    if (!st.notes.empty()) log("  " + st.notes);
    log("  Brushes are editable (rebuilt from collision). The render mesh is reference-only: "
        "View > Reference Mesh toggles it. Save writes brushes + entities to a .map.");
    return true;
}

// Reads, from the running game, the XModel named by every point entity's "model" key and draws it
// on that entity. Models stay loaded (and follow their entities) until reloaded; only models the
// game currently has loaded can be found.
void App::loadEntityModels() {
    std::vector<std::string> names;
    for (const Entity& e : doc.ents)
        if (const std::string* m = e.find("model"); m && !m->empty() && (*m)[0] != '*' && e.brushes.empty())
            names.push_back(*m);
    if (names.empty()) {
        log("No point entities with a \"model\" key in this map.");
        return;
    }
    live::LiveModelStats st;
    std::string err;
    log("Loading entity models from the running game ...");
    if (!live::loadLiveModels(names, liveModels, st, err)) {
        log("Loading models failed: " + err);
        MessageBoxA(hwnd, err.c_str(), "Load entity models", MB_OK | MB_ICONERROR);
        return;
    }
    std::map<std::string, std::string> hints;
    for (const auto& [key, model] : liveModels)
        for (const live::LiveModelSurf& s : model->surfs)
            if (!s.material.empty() && !s.image.empty()) hints.emplace(s.material, s.image);
    if (!hints.empty()) {
        assets.addMaterialHints(hints);
        renderer.clearTextures();
    }
    meshRevision = 0;  // redraw with the models
    log("Loaded from " + st.game + ": " + std::to_string(st.found) + " of " + std::to_string(st.requested) + " models (" +
        std::to_string(st.surfaces) + " surfaces, " + std::to_string(st.triangles) + " triangles)");
    if (!st.missing.empty()) {
        std::string list;
        for (size_t i = 0; i < st.missing.size() && i < 12; i++) list += (i ? ", " : "") + st.missing[i];
        if (st.missing.size() > 12) list += ", ...";
        log("  not loaded in the game right now (" + std::to_string(st.missing.size()) + "): " + list);
    }
}

bool App::importLiveModels() {
    // Additive: the dumped models merge into the current document as reference geometry, so they
    // land in the map already open (import a .ff first, then dump the running game's models onto it).
    bool wasEmpty = !doc.refMesh;
    live::LiveStats st;
    std::string err;
    log("Dumping models from the running game ...");
    if (!live::importFromProcess(doc, st, err)) {
        log("Live dump failed: " + err);
        MessageBoxA(hwnd, err.c_str(), "Import models from running game", MB_OK | MB_ICONERROR);
        return false;
    }
    meshRevision = 0;  // force a renderer rebuild
    if (doc.refMesh) renderer.showToolFaces = false;
    if (doc.refMesh && !doc.refMesh->materialImages.empty()) {
        assets.addMaterialHints(doc.refMesh->materialImages);
        renderer.clearTextures();
    }
    if (wasEmpty) frameAll();  // keep the camera put when adding onto an already-open map
    std::string summary = st.mapName + " (" + st.game + "): " + std::to_string(st.models) + " model instances (" +
                          std::to_string(st.uniqueModels) + " unique), " + std::to_string(st.surfaces) +
                          " surfaces / " + std::to_string(st.triangles) + " triangles, " +
                          std::to_string(st.materials) + " materials (" + std::to_string(st.images) +
                          " with an image)";
    log("Dumped " + summary);
    if (st.modelsSkipped) log("  " + std::to_string(st.modelsSkipped) + " instances skipped (null or unreadable)");
    log(std::string("  Merged into the ") + (wasEmpty ? "new document" : "open map") +
        " as reference-only geometry (View > Reference Mesh). Nothing was written to the game.");
    return true;
}

void App::importDialog() {
    if (!confirmDiscard()) return;
    std::string path = fileDialog(hwnd, false, lastImportPath, kFastfileFilter, "ff", "Import map from fastfile");
    if (path.empty()) return;
    lastImportPath = path;
    importFastfileMap(path);
}

void App::frameAll() {
    AABB box;
    for (const Entity& e : doc.ents)
        for (const Brush& b : e.brushes) box.add(b.bounds);
    if (doc.refMesh) box.add(doc.refMesh->bounds);
    if (!box.valid()) return;
    Vec3 c = box.center(), size = box.maxs - box.mins;
    double extent = std::max({size.x, size.y, size.z, 256.0});
    camera.pos = c + Vec3{-0.35 * extent, -0.35 * extent, 0.25 * extent};
    camera.yaw = 45;
    camera.pitch = -25;
    camSpeed = std::clamp(extent / 20.0, 400.0, 4000.0);
    for (int i = 0; i < 3; i++) {
        GridView& v = views[i];
        v.centerU = c[v.u];
        v.centerV = c[v.v];
        double span = std::max(size[v.u], size[v.v]);
        float px = std::max(gridRects[i].size.x, 400.0f);
        v.zoom = span > 0 ? std::clamp(px / span * 0.9, 0.002, 4.0) : 0.5;
    }
}

void App::dumpDialog() {
    std::string path = fileDialog(hwnd, false, lastDumpPath, kFastfileFilter, "ff", "Dump fastfile");
    if (path.empty()) return;
    lastDumpPath = path;
    zone::DumpResult r;
    std::string err;
    if (!zone::dumpFastfile(path, cacheDir + "\\dumps", r, err)) {
        log("Dump failed: " + err);
        MessageBoxA(hwnd, err.c_str(), "Dump failed", MB_OK | MB_ICONERROR);
        return;
    }
    log("Dumped " + path + " -> " + r.folder + ": " + r.summary);
    std::string msg = "Dumped to " + r.folder + "\n\n" + r.summary + "\n\n" + std::to_string(r.files.size()) +
                      " file(s) written (see report.txt).\n\nOpen the folder?";
    if (MessageBoxA(hwnd, msg.c_str(), "Dump fastfile", MB_YESNO | MB_ICONINFORMATION) == IDYES)
        ShellExecuteA(hwnd, "open", r.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::buildDemo() {
    // Chooses a real world material if the scan has found one, for a useful first view.
    std::string wall = "caulk", floorMat = "caulk";
    for (const MaterialEntry& m : assets.materials()) {
        if (m.prefix != "wc" || m.image.empty()) continue;
        if (wall == "caulk" && m.name.find("brick") != std::string::npos) wall = m.name;
        if (floorMat == "caulk" && m.name.find("concrete") != std::string::npos) floorMat = m.name;
    }
    doc.reset();
    doc.select(doc.addBrush(makeBox({-512, -384, 0}, {512, 384, 256}, wall)), false);
    doc.hollowSelected(16);
    doc.ents[0].brushes[4].setMaterial(floorMat);
    doc.addBrush(makeBox({-64, -64, 16}, {64, 64, 128}, floorMat));
    Brush ramp = makeBox({192, -256, 16}, {384, -64, 112}, wall);
    Face cut;
    cut.p[0] = {192, -256, 16};
    cut.p[1] = {192, -64, 16};
    cut.p[2] = {384, -256, 112};
    cut.material = wall;
    cut.tail = "lightmap_gray 16384 16384 0 0 0 0";
    ramp.faces.push_back(cut);
    ramp.rebuild();
    doc.addBrush(std::move(ramp));
    doc.addPointEntity("mp_tdm_spawn", {-352, 0, 16});
    doc.addPointEntity("mp_tdm_spawn", {-352, 128, 16});
    doc.addPointEntity("mp_global_intermission", {0, 256, 128});
    doc.addPointEntity("light", {128, 128, 200});
    doc.clearSelection();
    doc.select({0, 6}, false);
    doc.dirty = false;
    camera.pos = {-448, -320, 224};
    camera.yaw = 35;
    camera.pitch = -22;
    for (GridView& v : views) v.zoom = 0.28;
}

void App::selectAll() {
    doc.sel.clear();
    for (size_t e = 0; e < doc.ents.size(); e++) {
        for (size_t b = 0; b < doc.ents[e].brushes.size(); b++)
            doc.sel.push_back({static_cast<int>(e), static_cast<int>(b)});
        if (e != 0 && doc.ents[e].brushes.empty()) doc.sel.push_back({static_cast<int>(e), -1});
    }
    doc.touchSelection();
}

void App::cloneSelection() {
    if (doc.sel.empty()) return;
    doc.pushUndo();
    doc.cloneSelected({gridSize, gridSize, 0});
}

void App::hollowSelection() {
    if (doc.sel.empty()) return;
    doc.pushUndo();
    if (!doc.hollowSelected(gridSize)) {
        doc.undo();
        log("Hollow needs brushes larger than twice the grid size");
    }
}

void App::deleteSelection() {
    if (doc.sel.empty()) return;
    doc.pushUndo();
    doc.deleteSelected();
}

void App::applyMaterial(const std::string& name) {
    currentMaterial = name;
    bool anyBrush = std::any_of(doc.sel.begin(), doc.sel.end(), [](const Sel& s) { return s.brush >= 0; });
    if (!anyBrush) return;
    doc.pushUndo();
    doc.setSelectedMaterial(name);
}

void App::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || dragMode != DragMode::None) return;
    bool ctrl = io.KeyCtrl;
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_T, false)) {
        terrainSculpt = !terrainSculpt && activeTerrain() >= 0;
        log(terrainSculpt ? "Terrain sculpting: hold the left mouse button on the terrain (Ctrl inverts)" : "Terrain sculpting off");
    }
    // Pen tool keys (before the general ones: Backspace/Esc mean "undo point"/"cancel" here).
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_P, false)) {
        penActive = !penActive;
        penPoints.clear();
        penView = -1;
        log(penActive ? "Pen: click points in a grid view; Enter or the first point finishes" : "Pen off");
    }
    if (penActive) {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
            finishPen();
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true) && !penPoints.empty()) {
            penPoints.pop_back();
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (penPoints.empty()) {
                penActive = false;
                log("Pen off");
            }
            penPoints.clear();
            return;
        }
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, true)) doc.undo();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, true)) doc.redo();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) save(io.KeyShift);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) openDialog();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) newMap();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) selectAll();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) cloneSelection();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_H, false)) hollowSelection();
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) deleteSelection();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) doc.clearSelection();
    if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) requestCompile(PendingCompile::ToFile);
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) requestCompile(PendingCompile::AndRun);
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) startBake();
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) {
        renderer.cullBackFaces = !renderer.cullBackFaces;
        meshRevision = 0;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) frameAll();
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true)) gridSize = std::max(1.0, gridSize / 2);
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true)) gridSize = std::min(512.0, gridSize * 2);
}

void App::refreshMaterials() {
    uint64_t v = assets.materialsVersion();
    if (v != materialsSeen) {
        materialsSeen = v;
        materials = assets.materials();
        filterDirty = true;
    }
    if (filterDirty) {
        filterDirty = false;
        filtered.clear();
        for (int i = 0; i < static_cast<int>(materials.size()); i++) {
            const MaterialEntry& m = materials[i];
            if (worldOnly && m.prefix != "wc") continue;
            if (containsNoCase(m.name, filterText)) filtered.push_back(i);
        }
    }
}

void App::frame() {
    if (meshRevision != doc.revision) {
        renderer.rebuild(doc);
        renderer.setVolumetric(light::readVolumetric(doc));  // worldspawn volumetric_* edits show right away
        meshRevision = doc.revision;
    }
    renderer.pumpTextures(8);
    pollBake();
    updateFxPreview();
    refreshMaterials();
    handleShortcuts();
    drawMenu();
    drawToolbar();
    drawDockspace();
    for (int i = 0; i < 3; i++) drawGrid(views[i], i);
    drawCamera();
    drawInspector();
    drawTerrainPanel();
    drawTextures();
    drawModelBrowser();
    drawConsole();
    if (showSettings) drawSettings();
    drawCompileTargetPopup();
    if (terrainStroking && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) endTerrainStroke();
    if (!focusWindow.empty() && !firstFrame) {
        ImGui::SetWindowFocus(focusWindow.c_str());
        focusWindow.clear();
    }
    firstFrame = false;
}

void App::drawDockspace() {
    ImGuiID id = ImGui::GetID("MainDock");
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockSpaceOverViewport(id, vp);
    if (!firstFrame || ImGui::DockBuilderGetNode(id)->IsSplitNode()) return;

    // Default layout, only when no saved layout exists: the classic Radiant four-pane plus tools.
    ImGui::DockBuilderRemoveNode(id);
    ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(id, vp->WorkSize);
    ImGuiID right, left, bottom, topLeft, botLeft, topRight, botRight, inspector;
    ImGui::DockBuilderSplitNode(id, ImGuiDir_Right, 0.24f, &right, &left);
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.20f, &bottom, &left);
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Left, 0.5f, &topLeft, &topRight);
    ImGui::DockBuilderSplitNode(topLeft, ImGuiDir_Down, 0.5f, &botLeft, &topLeft);
    ImGui::DockBuilderSplitNode(topRight, ImGuiDir_Down, 0.5f, &botRight, &topRight);
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.42f, &inspector, &right);
    ImGui::DockBuilderDockWindow("Camera", topRight);
    ImGui::DockBuilderDockWindow("Top (XY)", topLeft);
    ImGui::DockBuilderDockWindow("Front (XZ)", botLeft);
    ImGui::DockBuilderDockWindow("Side (YZ)", botRight);
    ImGui::DockBuilderDockWindow("Inspector", inspector);
    ImGui::DockBuilderDockWindow("Textures", right);
    ImGui::DockBuilderDockWindow("Models", right);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderFinish(id);
}

void App::drawMenu() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", "Ctrl+N")) newMap();
        if (ImGui::MenuItem("Open...", "Ctrl+O")) openDialog();
        if (ImGui::MenuItem("Save", "Ctrl+S")) save(false);
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) save(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Import Map from .ff...")) importDialog();
        if (ImGui::MenuItem("Import Models from Running Game")) importLiveModels();
        if (ImGui::MenuItem("Load Entity Models from Running Game")) loadEntityModels();
        if (ImGui::MenuItem("Compile to .ff...", "F7")) requestCompile(PendingCompile::ToFile);
        if (ImGui::MenuItem("Compile & Run in Game", "F5")) requestCompile(PendingCompile::AndRun);
        if (ImGui::MenuItem("Edit Map Script...")) editMapScript();
        if (ImGui::MenuItem("Reload zone_source Models", nullptr, false, !doc.path.empty())) loadZoneSourcePreview();
        if (ImGui::MenuItem("Dump Fast File...")) dumpDialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) PostMessageA(hwnd, WM_CLOSE, 0, 0);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, doc.canUndo())) doc.undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, doc.canRedo())) doc.redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Clone", "Ctrl+D", false, !doc.sel.empty())) cloneSelection();
        if (ImGui::MenuItem("Hollow", "Ctrl+H", false, !doc.sel.empty())) hollowSelection();
        if (ImGui::MenuItem("Delete", "Del", false, !doc.sel.empty())) deleteSelection();
        if (ImGui::MenuItem("Select All", "Ctrl+A")) selectAll();
        ImGui::Separator();
        if (ImGui::BeginMenu("Brushes to Entity", !doc.sel.empty())) {
            for (const EntityClass& c : entityClasses()) {
                if (c.brush && ImGui::MenuItem(c.name)) {
                    doc.pushUndo();
                    doc.selectedToEntity(c.name);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Settings...")) showSettings = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Reference Mesh", nullptr, &renderer.showRefMesh, doc.refMesh != nullptr);
        if (ImGui::MenuItem("Brushes", nullptr, &renderer.showBrushes)) meshRevision = 0;
        if (ImGui::MenuItem("Tool Brushes (caulk, clip...)", nullptr, &renderer.showToolFaces)) meshRevision = 0;
        if (ImGui::MenuItem("Back-Face Culling (as in game)", "F6", &renderer.cullBackFaces)) meshRevision = 0;
        if (ImGui::MenuItem("Frame All", "Home")) frameAll();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Lighting")) {
        const bool running = bakeThread.joinable();
        if (ImGui::MenuItem("Bake Lighting", "F9", false, !running)) startBake();
        if (ImGui::MenuItem("Cancel Bake", nullptr, false, running)) bakeProgress.cancel = true;
        if (ImGui::MenuItem("Show Lighting", nullptr, &renderer.showLighting, renderer.hasLighting())) meshRevision = 0;
        ImGui::MenuItem("Show Volumetrics (shafts, fog)", nullptr, &renderer.showVolumetrics, renderer.hasLighting());
        if (ImGui::MenuItem("Effects Preview", nullptr, showEffects)) {
            showEffects = !showEffects;
            if (showEffects) startFxPreview("turned on");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Plays the fx_origin effects (zone_source/fx) in the 3D view.\nStarts by itself after a bake (F9) or a compile.");
        if (ImGui::MenuItem("Reload Effects", nullptr, false, showEffects)) startFxPreview("reloaded");
        {
            const std::set<std::string> groups = showEffects ? fxPreview.groups() : std::set<std::string>{};
            if (ImGui::BeginMenu("Effect Groups", !groups.empty())) {
                // fx_origins with a targetname: what level notify("<name>") / ("<name>_stop") do in game
                for (const std::string& g : groups) {
                    ImGui::PushID(g.c_str());
                    ImGui::TextUnformatted(g.c_str());
                    ImGui::SameLine(160);
                    if (ImGui::SmallButton("Start")) fxPreview.startGroup(g);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Stop")) fxPreview.stopGroup(g);
                    ImGui::PopID();
                }
                ImGui::EndMenu();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("fx_origins with a targetname. They also start when the camera enters a\ntrigger_radius whose target is the name.");
        }
        if (ImGui::MenuItem("Add Light at Camera")) addLightAtCamera();
        ImGui::Separator();
        float lux = static_cast<float>(lightSettings.luxelSize);
        ImGui::SetNextItemWidth(160);
        if (ImGui::SliderFloat("Luxel size", &lux, 4, 64, "%.0f units")) lightSettings.luxelSize = lux;
        ImGui::SetNextItemWidth(160);
        ImGui::SliderInt("Sky rays", &lightSettings.skySamples, 8, 256);
        ImGui::SetNextItemWidth(160);
        ImGui::SliderInt("Bounces", &lightSettings.bounces, 0, 3);
        ImGui::Separator();
        ImGui::TextDisabled("Sun/sky: worldspawn sundirection, suncolor, sunlight,");
        ImGui::TextDisabled("diffusefraction, sundiffusecolor, ambient, _color");
        if (!bakeResult)
            ImGui::TextDisabled("Not baked yet");
        else if (bakeUpToDate())
            ImGui::TextDisabled("Bake is up to date");
        else
            ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "Lighting changed: rebake (F9)");
        ImGui::EndMenu();
    }
    drawWeaponsMenu();
    if (ImGui::BeginMenu("Grid")) {
        for (double g : {1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 128.0, 256.0, 512.0}) {
            char label[16];
            std::snprintf(label, sizeof label, "%g", g);
            if (ImGui::MenuItem(label, nullptr, gridSize == g)) gridSize = g;
        }
        ImGui::EndMenu();
    }
    char status[160];
    size_t brushCount = 0;
    for (const Entity& e : doc.ents) brushCount += e.brushes.size();
    int n = std::snprintf(status, sizeof status, "%s%s  |  grid %g  |  %zu brushes  |  %zu selected",
                          doc.path.empty() ? "untitled" : doc.path.substr(doc.path.find_last_of("\\/") + 1).c_str(),
                          doc.dirty ? " *" : "", gridSize, brushCount, doc.sel.size());
    if (assets.scanning() && n > 0 && n < (int)sizeof status)
        n += std::snprintf(status + n, sizeof status - n, "  |  scanning zones %d/%d", assets.zonesDone(), assets.zonesTotal());
    if (bakeThread.joinable() && n > 0 && n < (int)sizeof status)
        std::snprintf(status + n, sizeof status - n, "  |  baking lighting %d%%",
                      static_cast<int>(bakeProgress.fraction * 100));
    float w = ImGui::CalcTextSize(status).x + 16;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - w));
    ImGui::TextDisabled("%s", status);
    ImGui::EndMainMenuBar();
}

void App::pickInGrid(const GridView& view, double u, double v, bool additive) {
    Sel best{-1, -1};
    double bestArea = 1e30;
    auto consider = [&](const AABB& b, Sel s) {
        if (u < b.mins[view.u] || u > b.maxs[view.u] || v < b.mins[view.v] || v > b.maxs[view.v]) return;
        double area = (b.maxs[view.u] - b.mins[view.u]) * (b.maxs[view.v] - b.mins[view.v]);
        if (area < bestArea) { bestArea = area; best = s; }  // smallest first, so small things stay reachable
    };
    for (size_t e = 0; e < doc.ents.size(); e++) {
        const Entity& ent = doc.ents[e];
        for (size_t b = 0; b < ent.brushes.size(); b++)
            consider(ent.brushes[b].bounds, {static_cast<int>(e), static_cast<int>(b)});
        if (e != 0 && ent.brushes.empty()) consider(pointEntityBounds(ent), {static_cast<int>(e), -1});
    }
    if (best.ent >= 0) doc.select(best, additive);
    else if (!additive) doc.clearSelection();
}

void App::applyDrag(const GridView& view, double u, double v) {
    if (dragMode == DragMode::Create) {
        double u0 = snapTo(dragStartU, gridSize), v0 = snapTo(dragStartV, gridSize);
        double u1 = snapTo(u, gridSize), v1 = snapTo(v, gridSize);
        if (u1 == u0 || v1 == v0) return;  // too small to be a solid yet
        Vec3 mn = workMin, mx = workMax;
        mn[view.u] = std::min(u0, u1);
        mx[view.u] = std::max(u0, u1);
        mn[view.v] = std::min(v0, v1);
        mx[view.v] = std::max(v0, v1);
        Brush b = makeBox(mn, mx, currentMaterial);
        if (!dragMoved) {
            doc.pushUndo();
            doc.clearSelection();
            createdBrush = doc.addBrush(std::move(b));
            doc.sel = {createdBrush};
            dragMoved = true;
        } else {
            doc.ents[createdBrush.ent].brushes[createdBrush.brush] = std::move(b);
        }
        doc.touch();
        return;
    }

    double du = u - dragStartU, dv = v - dragStartV;
    if (dragMode == DragMode::Move) {
        Vec3 d;
        d[view.u] = snapTo(du, gridSize);
        d[view.v] = snapTo(dv, gridSize);
        if (!dragMoved && d.x == 0 && d.y == 0 && d.z == 0) return;
        if (!dragMoved) {
            doc.pushUndo();
            dragMoved = true;
        }
        for (const DragBase& base : dragBase) {
            if (base.sel.brush >= 0) {
                Brush b = base.brush;
                b.translate(d);
                doc.ents[base.sel.ent].brushes[base.sel.brush] = std::move(b);
            } else {
                doc.ents[base.sel.ent].setOrigin(base.origin + d);
            }
        }
        doc.touch();
    } else if (dragMode == DragMode::Resize) {
        // Moves the grabbed bounding edge to a grid line, then applies that shift to the faces.
        double amountU = 0, amountV = 0;
        if (resizeU) {
            double edge = resizeU > 0 ? dragBox.maxs[view.u] : dragBox.mins[view.u];
            amountU = snapTo(edge + du, gridSize) - edge;
        }
        if (resizeV) {
            double edge = resizeV > 0 ? dragBox.maxs[view.v] : dragBox.mins[view.v];
            amountV = snapTo(edge + dv, gridSize) - edge;
        }
        if (!dragMoved && amountU == 0 && amountV == 0) return;
        std::vector<Brush> next;
        for (const DragBase& base : dragBase) {
            if (base.sel.brush < 0) continue;
            Brush b = base.brush;
            if ((resizeU && amountU != 0 && !b.moveFaces(view.u, resizeU, amountU)) ||
                (resizeV && amountV != 0 && !b.moveFaces(view.v, resizeV, amountV)))
                return;  // would collapse a brush; holds the last valid size
            next.push_back(std::move(b));
        }
        if (!dragMoved) {
            doc.pushUndo();
            dragMoved = true;
        }
        size_t i = 0;
        for (const DragBase& base : dragBase)
            if (base.sel.brush >= 0) doc.ents[base.sel.ent].brushes[base.sel.brush] = std::move(next[i++]);
        doc.touch();
    }
}

void App::endDrag() {
    if (dragMode != DragMode::None && dragMoved) {
        AABB b = doc.selectionBounds();
        if (b.valid()) {
            workMin = b.mins;
            workMax = b.maxs;
        }
    } else if (dragMode == DragMode::Create) {
        pickInGrid(views[dragView], dragStartU, dragStartV, false);  // a click, not a drag
    }
    dragMode = DragMode::None;
    dragView = -1;
    dragBase.clear();
}

void App::drawGrid(GridView& view, int index) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    bool open = ImGui::Begin(view.title);
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::End();
        return;
    }
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 8 || size.y < 8) {
        ImGui::End();
        return;
    }
    ImGui::InvisibleButton("##grid", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 end{pos.x + size.x, pos.y + size.y};
    gridRects[index] = {pos, size};
    float cx = pos.x + size.x * 0.5f, cy = pos.y + size.y * 0.5f;

    auto toScreen = [&](double u, double v) {
        return ImVec2{cx + static_cast<float>((u - view.centerU) * view.zoom), cy - static_cast<float>((v - view.centerV) * view.zoom)};
    };
    auto worldU = [&](float x) { return view.centerU + (x - cx) / view.zoom; };
    auto worldV = [&](float y) { return view.centerV - (y - cy) / view.zoom; };

    double mu = worldU(io.MousePos.x), mv = worldV(io.MousePos.y);
    // A model dragged from the Models browser: placed under the cursor on the working plane.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("IW4R_XMODEL")) {
            Vec3 p;
            p[view.u] = snapTo(mu, gridSize);
            p[view.v] = snapTo(mv, gridSize);
            p[view.w] = workMin[view.w];
            placeModel(static_cast<const char*>(pl->Data), p);
        }
        ImGui::EndDragDropTarget();
    }

    // Zoom around the cursor, so the point under it stays put.
    if (hovered && io.MouseWheel != 0) {
        view.zoom = std::clamp(view.zoom * std::pow(1.2, io.MouseWheel), 0.02, 16.0);
        view.centerU += mu - worldU(io.MousePos.x);
        view.centerV += mv - worldV(io.MousePos.y);
        mu = worldU(io.MousePos.x);
        mv = worldV(io.MousePos.y);
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 3)) {
        view.centerU -= io.MouseDelta.x / view.zoom;
        view.centerV += io.MouseDelta.y / view.zoom;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        camera.pos[view.u] = snapTo(mu, gridSize);
        camera.pos[view.v] = snapTo(mv, gridSize);
    }

    // Finds what a left press would grab. Shared by the cursor hint and the press itself.
    AABB selBox = doc.selectionBounds();
    int grabU = 0, grabV = 0;
    bool insideSel = false;
    if (selBox.valid()) {
        bool canResize = std::any_of(doc.sel.begin(), doc.sel.end(), [](const Sel& s) { return s.brush >= 0; });
        ImVec2 a = toScreen(selBox.mins[view.u], selBox.maxs[view.v]);
        ImVec2 b = toScreen(selBox.maxs[view.u], selBox.mins[view.v]);
        float x = io.MousePos.x, y = io.MousePos.y;
        // Handles shrink with the object so the middle of a small brush can still be grabbed.
        float handleU = std::min(7.0f, (b.x - a.x) * 0.25f), handleV = std::min(7.0f, (b.y - a.y) * 0.25f);
        insideSel = x > a.x && x < b.x && y > a.y && y < b.y;
        if (canResize && x > a.x - handleU && x < b.x + handleU && y > a.y - handleV && y < b.y + handleV) {
            if (std::fabs(x - a.x) < handleU) grabU = -1;
            else if (std::fabs(x - b.x) < handleU) grabU = 1;
            if (std::fabs(y - a.y) < handleV) grabV = 1;
            else if (std::fabs(y - b.y) < handleV) grabV = -1;
        }
        if (grabU || grabV) insideSel = false;
    }

    if (hovered && dragMode == DragMode::None) {
        if (grabU && grabV) ImGui::SetMouseCursor(grabU == grabV ? ImGuiMouseCursor_ResizeNESW : ImGuiMouseCursor_ResizeNWSE);
        else if (grabU) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        else if (grabV) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        else if (insideSel) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }

    // Pen tool: left clicks place snapped outline points instead of selecting / dragging.
    const double penU = snapTo(mu, gridSize), penV = snapTo(mv, gridSize);
    bool penCloses = false;
    if (penActive && penView == index && penPoints.size() >= 3) {
        ImVec2 first = toScreen(penPoints[0][0], penPoints[0][1]);
        penCloses = std::hypot(io.MousePos.x - first.x, io.MousePos.y - first.y) < 9.0f;
    }
    if (terrainSculpt) {
        // Terrain sculpting in the Top view (the height field lies in XY).
        if (view.w == 2 && hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left) && (terrainStroking || ImGui::IsMouseClicked(ImGuiMouseButton_Left)))
            terrainStrokeAt(mu, mv);
    } else if (penActive) {
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (penView != index) {  // a new outline in this view
                penPoints.clear();
                penView = index;
            }
            const bool repeat = !penPoints.empty() && penPoints.back()[0] == penU && penPoints.back()[1] == penV;
            if (penCloses || (repeat && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && penPoints.size() >= 3)) finishPen();
            else if (!repeat) penPoints.push_back({penU, penV});
        }
    } else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (io.KeyShift) {
            pickInGrid(view, mu, mv, true);
        } else if (grabU || grabV || insideSel) {
            dragMode = insideSel ? DragMode::Move : DragMode::Resize;
            dragView = index;
            dragStartU = mu;
            dragStartV = mv;
            resizeU = grabU;
            resizeV = grabV;
            dragBox = selBox;
            dragMoved = false;
            dragBase.clear();
            for (const Sel& s : doc.sel) {
                DragBase base{s, {}, {}};
                if (s.brush >= 0) base.brush = doc.ents[s.ent].brushes[s.brush];
                else doc.ents[s.ent].origin(base.origin);
                dragBase.push_back(std::move(base));
            }
        } else {
            dragMode = DragMode::Create;
            dragView = index;
            dragStartU = mu;
            dragStartV = mv;
            dragMoved = false;
        }
    }
    if (dragView == index && dragMode != DragMode::None) {
        applyDrag(view, mu, mv);
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) endDrag();
    }

    if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).x == 0 &&
        ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).y == 0) {
        contextU = snapTo(mu, gridSize);
        contextV = snapTo(mv, gridSize);
        contextW = workMin[view.w];
        ImGui::OpenPopup("GridContext");
    }
    if (ImGui::BeginPopup("GridContext")) {
        ImGui::TextDisabled("Add entity");
        ImGui::Separator();
        for (const EntityClass& c : entityClasses()) {
            if (!c.brush && ImGui::MenuItem(c.name)) {
                Vec3 p;
                p[view.u] = contextU;
                p[view.v] = contextV;
                p[view.w] = contextW;
                doc.pushUndo();
                doc.sel = {doc.addPointEntity(c.name, p)};
                doc.touchSelection();
            }
        }
        ImGui::EndPopup();
    }

    dl->PushClipRect(pos, end, true);
    dl->AddRectFilled(pos, end, rgb(0.13f, 0.14f, 0.16f));

    // Hides lines that would be closer together than a few pixels.
    double step = gridSize;
    while (step * view.zoom < 6) step *= 2;
    double uMin = snapTo(worldU(pos.x), step) - step, uMax = worldU(end.x);
    double vMin = snapTo(worldV(end.y), step) - step, vMax = worldV(pos.y);
    ImU32 fine = rgb(0.21f, 0.23f, 0.26f), major = rgb(0.29f, 0.32f, 0.37f), axis = rgb(0.42f, 0.47f, 0.55f);
    double majorStep = std::max(step * 8, 64.0);
    for (double u = uMin; u <= uMax; u += step) {
        float x = toScreen(u, 0).x;
        dl->AddLine({x, pos.y}, {x, end.y}, u == 0 ? axis : (std::fmod(std::fabs(u), majorStep) < 0.5 ? major : fine));
    }
    for (double v = vMin; v <= vMax; v += step) {
        float y = toScreen(0, v).y;
        dl->AddLine({pos.x, y}, {end.x, y}, v == 0 ? axis : (std::fmod(std::fabs(v), majorStep) < 0.5 ? major : fine));
    }

    auto drawBrush = [&](const Brush& b, ImU32 col, float thick) {
        if (b.bounds.maxs[view.u] < uMin || b.bounds.mins[view.u] > uMax || b.bounds.maxs[view.v] < vMin || b.bounds.mins[view.v] > vMax) return;
        // Zoomed out over a whole imported map: sub-2px brushes are a dot, not ~30 lines each.
        double px = std::max(b.bounds.maxs[view.u] - b.bounds.mins[view.u], b.bounds.maxs[view.v] - b.bounds.mins[view.v]) * view.zoom;
        if (px < 2.0) {
            ImVec2 c = toScreen((b.bounds.mins[view.u] + b.bounds.maxs[view.u]) * 0.5, (b.bounds.mins[view.v] + b.bounds.maxs[view.v]) * 0.5);
            dl->AddRectFilled(c, {c.x + 1, c.y + 1}, col);
            return;
        }
        for (const Face& f : b.faces) {
            if (f.winding.size() < 3) continue;
            ImVec2 prev = toScreen(f.winding.back()[view.u], f.winding.back()[view.v]);
            for (const Vec3& p : f.winding) {
                ImVec2 cur = toScreen(p[view.u], p[view.v]);
                dl->AddLine(prev, cur, col, thick);
                prev = cur;
            }
        }
    };

    ImU32 normalCol = rgb(0.85f, 0.87f, 0.9f), selectedCol = rgb(1, 0.25f, 0.2f);
    for (int pass = 0; pass < 2; pass++) {  // selected things draw last, on top
        for (size_t e = 0; e < doc.ents.size(); e++) {
            const Entity& ent = doc.ents[e];
            const EntityClass* cls = findEntityClass(ent.classname());
            ImU32 col = (e != 0 && cls) ? rgb(cls->r, cls->g, cls->b) : normalCol;
            for (size_t b = 0; b < ent.brushes.size(); b++) {
                bool s = doc.isSelected({static_cast<int>(e), static_cast<int>(b)});
                if (s == (pass == 1)) drawBrush(ent.brushes[b], s ? selectedCol : col, s ? 1.6f : 1.0f);
            }
            if (e != 0 && ent.brushes.empty()) {
                bool s = doc.isSelected({static_cast<int>(e), -1});
                if (s != (pass == 1)) continue;
                AABB box = pointEntityBounds(ent);
                ImVec2 a = toScreen(box.mins[view.u], box.maxs[view.v]), c = toScreen(box.maxs[view.u], box.mins[view.v]);
                float r = cls ? cls->r : 0.8f, g = cls ? cls->g : 0.3f, bl = cls ? cls->b : 0.8f;
                dl->AddRectFilled(a, c, rgb(r, g, bl, 0.25f));
                dl->AddRect(a, c, s ? selectedCol : rgb(r, g, bl), 0, 0, s ? 1.6f : 1.0f);
                if (view.zoom > 0.25) dl->AddText({a.x, a.y - 15}, rgb(r, g, bl, 0.9f), ent.classname().c_str());
            }
        }
    }

    if (terrainSculpt && view.w == 2 && hovered) {
        const ImVec2 c = toScreen(mu, mv);
        dl->AddCircle(c, static_cast<float>(terrainBrush.radius * view.zoom), rgb(0.5f, 1.0f, 0.45f), 48, 1.5f);
        dl->AddText({c.x + 8, c.y + 8}, rgb(0.6f, 1.0f, 0.55f), terrain::toolName(terrainBrush.tool));
    }
    // Pen outline: placed points, the rubber band to the snapped cursor, and the closing edge.
    if (penActive && (penView == index || (penPoints.empty() && hovered))) {
        const ImU32 penCol = rgb(1.0f, 0.4f, 0.75f), faint = rgb(1.0f, 0.4f, 0.75f, 0.45f);
        const ImVec2 cur = toScreen(penU, penV);
        for (size_t i = 0; i + 1 < penPoints.size(); i++)
            dl->AddLine(toScreen(penPoints[i][0], penPoints[i][1]), toScreen(penPoints[i + 1][0], penPoints[i + 1][1]), penCol, 2.0f);
        if (!penPoints.empty() && hovered && penView == index) {
            const ImVec2 last = toScreen(penPoints.back()[0], penPoints.back()[1]);
            const ImVec2 first = toScreen(penPoints[0][0], penPoints[0][1]);
            dl->AddLine(last, penCloses ? first : cur, penCol, 1.5f);
            if (!penCloses && penPoints.size() >= 2) dl->AddLine(cur, first, faint, 1.0f);
        } else if (penPoints.size() >= 3) {
            dl->AddLine(toScreen(penPoints.back()[0], penPoints.back()[1]), toScreen(penPoints[0][0], penPoints[0][1]), faint, 1.0f);
        }
        for (size_t i = 0; i < penPoints.size(); i++) {
            const ImVec2 q = toScreen(penPoints[i][0], penPoints[i][1]);
            dl->AddCircleFilled(q, i == 0 ? (penCloses ? 7.0f : 5.0f) : 3.5f, i == 0 ? rgb(1, 0.85f, 0.95f) : penCol);
        }
        if (hovered) {
            dl->AddCircle(cur, 5.0f, penCol, 12, 1.5f);
            const char* hint = penPoints.empty()       ? "pen: click to start an outline"
                               : penPoints.size() < 3 ? "pen: keep clicking points"
                               : penCloses            ? "pen: click to finish"
                                                      : "pen: click the first point or Enter to finish, Backspace undo, Esc cancel";
            dl->AddText({cur.x + 10, cur.y + 8}, rgb(1, 0.75f, 0.9f), hint);
        }
    }

    if (selBox.valid()) {
        ImVec2 a = toScreen(selBox.mins[view.u], selBox.maxs[view.v]), c = toScreen(selBox.maxs[view.u], selBox.mins[view.v]);
        dl->AddRect(a, c, rgb(1, 0.45f, 0.3f, 0.5f));
        char dims[48];
        std::snprintf(dims, sizeof dims, "%g x %g", selBox.maxs[view.u] - selBox.mins[view.u], selBox.maxs[view.v] - selBox.mins[view.v]);
        dl->AddText({a.x, c.y + 3}, rgb(1, 0.7f, 0.6f), dims);
    }

    // Camera marker, pointing where the camera faces.
    ImVec2 camPos = toScreen(camera.pos[view.u], camera.pos[view.v]);
    Vec3 fwd = camera.forward();
    float fu = static_cast<float>(fwd[view.u]), fv = static_cast<float>(-fwd[view.v]);
    float len = std::sqrt(fu * fu + fv * fv);
    if (len > 0.01f) {
        fu /= len;
        fv /= len;
        dl->AddTriangleFilled({camPos.x + fu * 13, camPos.y + fv * 13}, {camPos.x - fu * 6 - fv * 7, camPos.y - fv * 6 + fu * 7},
                              {camPos.x - fu * 6 + fv * 7, camPos.y - fv * 6 - fu * 7}, rgb(1, 0.85f, 0.15f));
    } else {
        dl->AddCircleFilled(camPos, 6, rgb(1, 0.85f, 0.15f));
    }

    static const char* names = "XYZ";
    char label[32];
    if (ImGui::IsMousePosValid()) std::snprintf(label, sizeof label, "%c/%c  %.0f %.0f", names[view.u], names[view.v], snapTo(mu, gridSize), snapTo(mv, gridSize));
    else std::snprintf(label, sizeof label, "%c/%c", names[view.u], names[view.v]);
    dl->AddText({pos.x + 6, pos.y + 4}, rgb(0.6f, 0.65f, 0.72f), label);
    dl->PopClipRect();
    ImGui::End();
}

void App::drawCamera() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    bool open = ImGui::Begin("Camera");
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::End();
        return;
    }
    ImVec2 size = ImGui::GetContentRegionAvail();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    int w = static_cast<int>(size.x), h = static_cast<int>(size.y);
    cameraRect = {pos, size};
    if (w > 0 && h > 0) {
        renderer.render(camera, w, h);
        if (ID3D11ShaderResourceView* srv = renderer.output()) ImGui::Image((ImTextureID)(intptr_t)srv, size);
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("##cam", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        ImGuiIO& io = ImGui::GetIO();
        bool hovered = ImGui::IsItemHovered();
        // A model dragged from the Models browser: placed where the cursor's ray hits a brush.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("IW4R_XMODEL")) {
                double ys = std::tan(70.0 * 3.14159265358979323846 / 360.0);
                double nx = ((io.MousePos.x - pos.x) / size.x * 2 - 1) * ys * (size.x / size.y);
                double ny = (1 - (io.MousePos.y - pos.y) / size.y * 2) * ys;
                Vec3 dir = normalize(camera.forward() + camera.right() * nx + camera.up() * ny);
                Vec3 hit;
                Vec3 p = cameraRayHit(dir, hit) ? hit : camera.pos + dir * 256.0;
                placeModel(static_cast<const char*>(pl->Data), {snapTo(p.x, gridSize), snapTo(p.y, gridSize), snapTo(p.z, gridSize)});
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            camera.yaw -= io.MouseDelta.x * 0.22;
            camera.pitch = std::clamp(camera.pitch - io.MouseDelta.y * 0.22, -89.0, 89.0);
            double speed = camSpeed * (io.KeyShift ? 3.0 : 1.0) * io.DeltaTime;
            Vec3 f = camera.forward(), r = camera.right();
            if (ImGui::IsKeyDown(ImGuiKey_W)) camera.pos += f * speed;
            if (ImGui::IsKeyDown(ImGuiKey_S)) camera.pos += f * -speed;
            if (ImGui::IsKeyDown(ImGuiKey_D)) camera.pos += r * speed;
            if (ImGui::IsKeyDown(ImGuiKey_A)) camera.pos += r * -speed;
            if (ImGui::IsKeyDown(ImGuiKey_E)) camera.pos.z += speed;
            if (ImGui::IsKeyDown(ImGuiKey_Q)) camera.pos.z -= speed;
        }
        if (hovered && io.MouseWheel != 0) camera.pos += camera.forward() * (io.MouseWheel * 64.0);
        terrainCursorValid = false;
        if (terrainSculpt && hovered) {
            // Sculpting: the cursor's ray against the terrain; the brush ring drawn on the ground.
            double ys = std::tan(70.0 * 3.14159265358979323846 / 360.0);
            double nx = ((io.MousePos.x - pos.x) / size.x * 2 - 1) * ys * (size.x / size.y);
            double ny = (1 - (io.MousePos.y - pos.y) / size.y * 2) * ys;
            Vec3 dir = normalize(camera.forward() + camera.right() * nx + camera.up() * ny);
            double hx, hy;
            if (terrainRayHit(dir, hx, hy)) {
                terrainCursorValid = true;
                terrainCursor[0] = hx, terrainCursor[1] = hy;
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && (terrainStroking || ImGui::IsMouseClicked(ImGuiMouseButton_Left)))
                    terrainStrokeAt(hx, hy);
                terrain::Field tf;
                const int te = activeTerrain();
                if (te >= 0 && (terrainStroking ? (tf = terrainField, true) : terrain::read(doc.ents[te], tf))) {
                    ImDrawList* cdl = ImGui::GetWindowDrawList();
                    const Vec3 f = camera.forward(), r = camera.right(), u = camera.up();
                    std::vector<ImVec2> ring;
                    for (int k = 0; k <= 48; k++) {
                        const double a = 2 * 3.14159265358979323846 * k / 48;
                        const double px = hx + std::cos(a) * terrainBrush.radius, py = hy + std::sin(a) * terrainBrush.radius;
                        double ph = 0;
                        if (!tf.heightAt(px, py, ph)) tf.heightAt(std::clamp(px, tf.worldX(0), tf.worldX(tf.nx)), std::clamp(py, tf.worldY(0), tf.worldY(tf.ny)), ph);
                        const Vec3 d = Vec3{px, py, tf.origin.z + ph + 2} - camera.pos;
                        const double z = dot(d, f);
                        if (z < 1) continue;
                        ring.push_back({pos.x + static_cast<float>((dot(d, r) / (z * ys * (size.x / size.y)) + 1) * 0.5 * size.x),
                                        pos.y + static_cast<float>((1 - dot(d, u) / (z * ys)) * 0.5 * size.y)});
                    }
                    if (ring.size() > 2) cdl->AddPolyline(ring.data(), static_cast<int>(ring.size()), IM_COL32(130, 255, 115, 255), 0, 2.0f);
                }
            }
        }
        if (!terrainSculpt && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            // Builds the view ray through the cursor to pick the nearest brush.
            double ys = std::tan(70.0 * 3.14159265358979323846 / 360.0);
            double nx = ((io.MousePos.x - pos.x) / size.x * 2 - 1) * ys * (size.x / size.y);
            double ny = (1 - (io.MousePos.y - pos.y) / size.y * 2) * ys;
            Vec3 dir = normalize(camera.forward() + camera.right() * nx + camera.up() * ny);
            double bestT = 1e30;
            Sel best{-1, -1};
            for (size_t e = 0; e < doc.ents.size(); e++) {
                for (size_t b = 0; b < doc.ents[e].brushes.size(); b++) {
                    double t;
                    int face;
                    if (rayBrush(doc.ents[e].brushes[b], camera.pos, dir, t, face) && t < bestT) {
                        bestT = t;
                        best = {static_cast<int>(e), static_cast<int>(b)};
                    }
                }
            }
            if (best.ent >= 0) doc.select(best, io.KeyShift);
            else if (!io.KeyShift) doc.clearSelection();
        }
    }
    ImGui::End();
}

void App::drawInspector() {
    if (!ImGui::Begin("Inspector")) {
        ImGui::End();
        return;
    }
    // A single selected entity is edited directly; a brush edits its owning entity's keys.
    Entity* target = nullptr;
    if (doc.sel.empty()) {
        target = &doc.ents[0];
    } else {
        int ent = doc.sel[0].ent;
        bool same = std::all_of(doc.sel.begin(), doc.sel.end(), [&](const Sel& s) { return s.ent == ent; });
        if (same) target = &doc.ents[ent];
    }

    if (!target) {
        ImGui::TextDisabled("Multiple entities selected");
    } else {
        ImGui::SeparatorText(target->classname().empty() ? "entity" : target->classname().c_str());
        int removeIndex = -1;
        if (ImGui::BeginTable("kv", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthStretch, 0.4f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
            ImGui::TableSetupColumn("x", ImGuiTableColumnFlags_WidthFixed, 22);
            for (size_t i = 0; i < target->kv.size(); i++) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(target->kv[i].key.c_str());
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                char buf[512];
                strncpy_s(buf, target->kv[i].value.c_str(), _TRUNCATE);
                bool changed = ImGui::InputText("##v", buf, sizeof buf);
                if (ImGui::IsItemActivated()) doc.armUndo();
                if (changed) {
                    target->kv[i].value = buf;
                    doc.touch();
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) doc.commitArmedUndo();
                ImGui::TableNextColumn();
                bool locked = target->kv[i].key == "classname";
                ImGui::BeginDisabled(locked);
                if (ImGui::SmallButton("x")) removeIndex = static_cast<int>(i);
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (removeIndex >= 0) {
            doc.pushUndo();
            target->kv.erase(target->kv.begin() + removeIndex);
            doc.touch();
        }
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.38f);
        ImGui::InputTextWithHint("##newk", "key", newKey, sizeof newKey);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 46);
        ImGui::InputTextWithHint("##newv", "value", newValue, sizeof newValue);
        ImGui::SameLine();
        if (ImGui::Button("Add") && newKey[0]) {
            doc.pushUndo();
            target->set(newKey, newValue);
            doc.touch();
            newKey[0] = newValue[0] = 0;
        }
    }

    // Brush properties apply across the whole selection.
    Brush* first = nullptr;
    int brushes = 0;
    for (const Sel& s : doc.sel) {
        if (s.brush >= 0) {
            if (!first) first = &doc.ents[s.ent].brushes[s.brush];
            brushes++;
        }
    }
    if (first) {
        char title[32];
        std::snprintf(title, sizeof title, brushes > 1 ? "%d brushes" : "brush", brushes);
        ImGui::SeparatorText(title);
        AABB box = doc.selectionBounds();
        ImGui::Text("Size  %g x %g x %g", box.maxs.x - box.mins.x, box.maxs.y - box.mins.y, box.maxs.z - box.mins.z);
        ImGui::Text("Mins  %g %g %g", box.mins.x, box.mins.y, box.mins.z);
        ImGui::Text("Material  %s", first->faces[0].material.c_str());
        // Per-brush cull toggle: two-sided brushes are visible from inside too (e.g. dust volumes).
        bool twoSided = first->twoSided();
        if (ImGui::Checkbox("Two-sided (visible from inside)", &twoSided)) {
            doc.pushUndo();
            for (const Sel& s : doc.sel)
                if (s.brush >= 0) doc.ents[s.ent].brushes[s.brush].setTwoSided(twoSided);
            doc.touch();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The game hides faces seen from behind. Two-sided brushes are compiled with a\n"
                              "mirrored copy of every face, so they also show from inside the brush.\n"
                              "Preview with View > Back-Face Culling (F6).");
        // Per-brush surface type (footsteps, impacts, bullettrace surfacetype, AI groundtype).
        {
            const std::string cur = first->surfaceType();
            bool mixed = false;
            for (const Sel& s : doc.sel)
                if (s.brush >= 0 && doc.ents[s.ent].brushes[s.brush].surfaceType() != cur) mixed = true;
            const std::string preview = mixed ? "(mixed)" : cur.empty() ? "From material" : cur;
            if (ImGui::BeginCombo("Surface type", preview.c_str())) {
                auto pick = [&](const std::string& v) {
                    doc.pushUndo();
                    for (const Sel& s : doc.sel)
                        if (s.brush >= 0) doc.ents[s.ent].brushes[s.brush].setSurfaceType(v);
                    doc.touch();
                };
                if (ImGui::Selectable("From material", !mixed && cur.empty())) pick("");
                ImGui::Separator();
                for (const std::string& n : surfaceTypeNames())
                    if (ImGui::Selectable(n.c_str(), !mixed && cur == n)) pick(n);
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("What the game thinks this brush is made of: footstep sounds, bullet impact\n"
                                  "effects and sounds, and script traces (bullettrace \"surfacetype\").\n"
                                  "From material = the stock material's own type (default for custom ones).");
        }
        TexDef t = first->faces[0].tex;
        float vals[5] = {(float)t.w, (float)t.h, (float)t.sx, (float)t.sy, (float)t.rot};
        bool changed = false;
        changed |= ImGui::DragFloat2("Tex size", vals, 1, 1, 8192, "%.0f");
        if (ImGui::IsItemActivated()) doc.armUndo();
        if (ImGui::IsItemDeactivatedAfterEdit()) doc.commitArmedUndo();
        changed |= ImGui::DragFloat2("Tex shift", vals + 2, 1, 0, 0, "%.0f");
        if (ImGui::IsItemActivated()) doc.armUndo();
        if (ImGui::IsItemDeactivatedAfterEdit()) doc.commitArmedUndo();
        changed |= ImGui::DragFloat("Tex rotate", vals + 4, 1, -360, 360, "%.0f");
        if (ImGui::IsItemActivated()) doc.armUndo();
        if (ImGui::IsItemDeactivatedAfterEdit()) doc.commitArmedUndo();
        if (changed) {
            for (const Sel& s : doc.sel) {
                if (s.brush < 0) continue;
                for (Face& f : doc.ents[s.ent].brushes[s.brush].faces) f.tex = {vals[0], vals[1], vals[2], vals[3], vals[4]};
            }
            doc.touch();
        }
    }
    ImGui::End();
}

void App::drawTextures() {
    if (!ImGui::Begin("Textures")) {
        ImGui::End();
        return;
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 96);
    if (ImGui::InputTextWithHint("##filter", "search materials", filterText, sizeof filterText)) filterDirty = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("World", &worldOnly)) filterDirty = true;
    ImGui::TextDisabled("%zu materials  |  current: %s", filtered.size(), currentMaterial.c_str());

    if (ImGui::BeginChild("grid")) {
        float avail = ImGui::GetContentRegionAvail().x;
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        int columns = std::max(1, static_cast<int>((avail + spacing) / (thumbSize + spacing)));
        float cell = thumbSize + ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y + 8;
        int tools = filterText[0] ? 0 : IM_ARRAYSIZE(kToolMaterials);
        int total = tools + static_cast<int>(filtered.size());
        int rows = (total + columns - 1) / columns;
        ImGuiListClipper clip;  // only visible rows are built, so only those load textures
        clip.Begin(rows, cell);
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                for (int c = 0; c < columns; c++) {
                    int i = r * columns + c;
                    if (i >= total) break;
                    std::string name = i < tools ? kToolMaterials[i] : materials[filtered[i - tools]].name;
                    if (c) ImGui::SameLine();
                    ImGui::BeginGroup();
                    ImGui::PushID(i);
                    ID3D11ShaderResourceView* srv = renderer.texture(name);
                    bool active = name == currentMaterial;
                    if (active) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(230, 80, 60, 255));
                    if (ImGui::ImageButton("##t", (ImTextureID)(intptr_t)srv, {thumbSize, thumbSize})) applyMaterial(name);
                    if (active) ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", name.c_str());
                    if (i < 2) {
                        ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                        thumbCenters[i] = {(mn.x + mx.x) / 2, (mn.y + mx.y) / 2};
                    }
                    ImGui::PushClipRect(ImGui::GetCursorScreenPos(),
                                        {ImGui::GetCursorScreenPos().x + thumbSize + 6, ImGui::GetCursorScreenPos().y + 20}, true);
                    ImGui::TextUnformatted(name.c_str());
                    ImGui::PopClipRect();
                    ImGui::PopID();
                    ImGui::EndGroup();
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// The tool bar under the menu (an ImGui viewport side bar, so the dock space shrinks to fit).
void App::drawToolbar() {
    const float h = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::BeginViewportSideBar("##iw4r_toolbar", ImGui::GetMainViewport(), ImGuiDir_Up, h, flags)) {
        const bool haveSource = !doc.path.empty();
        ImGui::BeginDisabled(!haveSource);
        if (ImGui::Button("TGA -> PNG")) convertTgaToPng();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Converts every .tga under zone_source (images, xmodels, ...) to a .png next to it.\n%s",
                              haveSource ? "" : "Save the map first: zone_source is next to the .map.");
        ImGui::SameLine();
        ImGui::Checkbox("delete .tga", &deleteTgaAfterConvert);
        ImGui::SameLine();
        if (ImGui::Button("Fix skeletons for IW4")) convertSkeletons();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Black Ops / CoD4 models in zone_source/xmodels -> MW2 skeletons:\n"
                              "lower-case bone names, tag_origin root and the tags MW2's AI/weapons use\n"
                              "(tag_eye, tag_weapon_right, ...), at most 128 bones (bones MW2 never animates\n"
                              "are merged into their parents). Originals are kept as .orig.");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (penActive) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(penActive ? "Pen (on)" : "Pen")) {
            penActive = !penActive;
            penPoints.clear();
            penView = -1;
        }
        if (penActive) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Pen (P): click points in a grid view to outline a shape; click the first point or press\n"
                              "Enter to make it. It's extruded through the work area's depth (the last selection's), split\n"
                              "into convex brushes if needed. Backspace removes the last point, Esc cancels / turns it off.");
        ImGui::SameLine();
        for (shapes::Shape s : shapes::kAll) {
            if (ImGui::Button(shapes::name(s))) addShape(static_cast<int>(s));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\nFits the selected brushes (replacing them), or a 128 box where the camera looks.\n"
                                  "Material: the current texture (%s).",
                                  shapes::tooltip(s), currentMaterial.c_str());
            ImGui::SameLine();
        }
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::DragInt("Sides", &shapeParams.sides, 0.2f, 3, 64);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::DragInt("Steps", &shapeParams.steps, 0.2f, 1, 128);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        float t = static_cast<float>(shapeParams.thickness);
        if (ImGui::DragFloat("Thickness", &t, 0.5f, 1, 512, "%.0f")) shapeParams.thickness = t;
    }
    ImGui::End();
}

void App::convertTgaToPng() {
    namespace fs = std::filesystem;
    const fs::path dir = zoneSourcePath(doc.path);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        log("TGA -> PNG: no zone_source folder next to the map (" + dir.string() + ")");
        return;
    }
    size_t converted = 0, skipped = 0, failed = 0;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".tga") continue;
        fs::path png = it->path();
        png.replace_extension(".png");
        if (fs::exists(png, ec) && fs::last_write_time(png, ec) >= fs::last_write_time(it->path(), ec)) {
            skipped++;
            continue;
        }
        imageio::Rgba img;
        std::string err;
        if (!imageio::load(it->path().string(), img, err) || !imageio::savePng(png.string(), img, err)) {
            log("  " + it->path().filename().string() + ": " + err);
            failed++;
            continue;
        }
        converted++;
        if (deleteTgaAfterConvert) fs::remove(it->path(), ec);
    }
    log("TGA -> PNG: " + std::to_string(converted) + " converted, " + std::to_string(skipped) + " already up to date" +
        (failed ? ", " + std::to_string(failed) + " failed" : std::string()) + (deleteTgaAfterConvert ? " (.tga files deleted)" : ""));
    if (converted) loadZoneSourcePreview();
}

void App::convertSkeletons() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(zoneSourcePath(doc.path)) / "xmodels";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        log("Fix skeletons: no zone_source/xmodels next to the map");
        return;
    }
    size_t changed = 0, total = 0;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string ext = e.path().extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".xmodel_export") continue;
        total++;
        std::ifstream in(e.path(), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        xmodelexport::Model m;
        std::string err;
        if (!xmodelexport::parse(text, m, err)) {
            log("  " + e.path().filename().string() + ": " + err);
            continue;
        }
        skeleton::Report r;
        r.source = skeleton::detectSource(text);
        if (!skeleton::convertForIw4(m, r, err)) {
            log("  " + e.path().filename().string() + ": " + err);
            continue;
        }
        log("  " + e.path().filename().string() + ": " + r.summary());
        if (!r.changed()) continue;
        // Keep the original once, and the export's comment header (it says which game it came from).
        fs::path orig = e.path();
        orig += ".orig";
        if (!fs::exists(orig, ec)) fs::copy_file(e.path(), orig, ec);
        std::vector<std::string> header;
        std::istringstream lines(text);
        for (std::string line; std::getline(lines, line) && line.rfind("//", 0) == 0;) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            header.push_back(line.substr(line.find_first_not_of("/ ")));
        }
        header.push_back("Skeleton converted for IW4 by iw4-radiant: " + r.summary());
        if (!xmodelexport::save(m, e.path().string(), err, header)) {
            log("  " + err);
            continue;
        }
        changed++;
    }
    log("Fix skeletons: " + std::to_string(changed) + " of " + std::to_string(total) + " model(s) converted" +
        (changed ? " (originals kept as .orig)" : ""));
    if (changed) loadZoneSourcePreview();
}

// ---- terrain ----
int App::activeTerrain() {
    // The selected terrain entity, else the last one edited, else the first in the map.
    for (const Sel& s : doc.sel)
        if (s.brush < 0 && s.ent > 0 && s.ent < static_cast<int>(doc.ents.size()) && doc.ents[s.ent].classname() == terrain::kClass)
            return terrainEnt = s.ent;
    if (terrainEnt > 0 && terrainEnt < static_cast<int>(doc.ents.size()) && doc.ents[terrainEnt].classname() == terrain::kClass)
        return terrainEnt;
    for (size_t e = 1; e < doc.ents.size(); e++)
        if (doc.ents[e].classname() == terrain::kClass) return terrainEnt = static_cast<int>(e);
    return terrainEnt = -1;
}

bool App::terrainRayHit(const Vec3& dir, double& x, double& y) {
    const int te = activeTerrain();
    terrain::Field f;
    if (te < 0 || !(terrainStroking ? (f = terrainField, true) : terrain::read(doc.ents[te], f))) return false;
    // March along the ray, then bisect the crossing.
    const double step = f.cell * 0.25;
    double prevT = 0;
    auto below = [&](double t, bool& inside) {
        const Vec3 p = camera.pos + dir * t;
        double h;
        inside = f.heightAt(p.x, p.y, h);
        return inside && p.z <= f.origin.z + h;
    };
    bool inside;
    for (double t = step; t < 40000; t += step) {
        if (below(t, inside)) {
            double a = prevT, b = t;
            for (int k = 0; k < 20; k++) {
                const double m = (a + b) / 2;
                (below(m, inside) ? b : a) = m;
            }
            const Vec3 p = camera.pos + dir * b;
            x = p.x, y = p.y;
            return true;
        }
        prevT = t;
    }
    return false;
}

void App::terrainStrokeAt(double x, double y) {
    const int te = activeTerrain();
    if (te < 0) return;
    if (!terrainStroking) {
        if (!terrain::read(doc.ents[te], terrainField)) return;
        doc.pushUndo();
        terrainStroking = true;
        double h = 0;
        terrainField.heightAt(x, y, h);
        terrainBrush.flattenTo = h;
        terrainBrush.seed++;
    }
    terrain::Stroke s = terrainBrush;
    if (s.tool == terrain::Tool::Paint) {
        s.material = currentMaterial;
        terrain::paint(doc, terrainField, s, x, y, ImGui::GetIO().KeyCtrl);
        return;
    }
    if (ImGui::GetIO().KeyCtrl) {  // Ctrl inverts raise / lower
        if (s.tool == terrain::Tool::Raise) s.tool = terrain::Tool::Lower;
        else if (s.tool == terrain::Tool::Lower) s.tool = terrain::Tool::Raise;
    }
    int i0, j0, i1, j1;
    const double dt = std::min(0.1, static_cast<double>(ImGui::GetIO().DeltaTime));
    if (terrain::apply(terrainField, s, x, y, dt, i0, j0, i1, j1)) terrain::rebuildCells(doc, terrainField, i0, j0, i1, j1);
}

void App::endTerrainStroke() {
    if (!terrainStroking) return;
    terrainStroking = false;
    const int te = activeTerrain();
    if (te >= 0) terrain::write(terrainField, doc.ents[te]);
    doc.touch();
}

// The Terrain panel: create a terrain, sculpt it (T), generate mountains, delete it.
void App::drawTerrainPanel() {
    if (ImGuiWindow* iw = ImGui::FindWindowByName("Inspector"); iw && iw->DockId)
        ImGui::SetNextWindowDockID(iw->DockId, ImGuiCond_FirstUseEver);  // a tab next to the Inspector
    if (!ImGui::Begin("Terrain")) {
        ImGui::End();
        return;
    }
    const int te = activeTerrain();
    terrain::Field f;
    const bool have = te >= 0 && terrain::read(doc.ents[te], f);
    ImGui::SeparatorText("Sculpt");
    if (!have) ImGui::TextDisabled("No terrain yet: create one below.");
    ImGui::BeginDisabled(!have);
    if (terrainSculpt) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(terrainSculpt ? "Sculpting (T)" : "Sculpt (T)", {-1, 0})) terrainSculpt = !terrainSculpt;
    if (terrainSculpt) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("While on: hold the left mouse button on the terrain in the 3D view or the Top view.\n"
                          "Ctrl inverts Raise/Lower and makes Paint erase. Each stroke is one undo step.");
    int k = 0;
    for (terrain::Tool t : terrain::kTools) {
        if (k++ % 3) ImGui::SameLine();
        if (ImGui::RadioButton(terrain::toolName(t), terrainBrush.tool == t)) terrainBrush.tool = t;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", terrain::toolTip(t));
    }
    float radius = static_cast<float>(terrainBrush.radius), strength = static_cast<float>(terrainBrush.strength);
    if (ImGui::SliderFloat("Radius", &radius, 16, 2048, "%.0f", ImGuiSliderFlags_Logarithmic)) terrainBrush.radius = radius;
    if (terrainBrush.tool != terrain::Tool::Paint) {
        if (ImGui::SliderFloat("Strength", &strength, 4, 1024, "%.0f", ImGuiSliderFlags_Logarithmic)) terrainBrush.strength = strength;
    } else {
        const bool bad = currentMaterial.empty() || currentMaterial.find("caulk") != std::string::npos;
        ImGui::TextColored(bad ? ImVec4(1, 0.5f, 0.4f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Text), "Texture: %s", currentMaterial.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The current texture: pick one in the Textures browser. Ctrl+drag erases paint.");
        float scatter = static_cast<float>(terrainBrush.scatter);
        if (ImGui::SliderFloat("Scatter", &scatter, 0, 1, "%.2f")) terrainBrush.scatter = scatter;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0: a hard circle. Higher: a ragged, scattered rim that looks less hand-drawn.");
        float s0 = static_cast<float>(terrainBrush.minSlope), s1 = static_cast<float>(terrainBrush.maxSlope);
        if (ImGui::DragFloatRange2("Only slopes", &s0, &s1, 0.25f, 0, 90, "%.0f deg", "%.0f deg", ImGuiSliderFlags_AlwaysClamp))
            terrainBrush.minSlope = s0, terrainBrush.maxSlope = s1;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Paint only triangles this steep, e.g. 35-90 puts rock on cliffs without touching the flats");
    }
    ImGui::EndDisabled();
    if (have) {
        ImGui::SeparatorText("This terrain");
        ImGui::TextDisabled("%s: %d x %d cells of %g (%d brushes), %s", f.id.c_str(), f.nx, f.ny, f.cell, f.nx * f.ny * 2, f.material.c_str());
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Height", &terrainMountain, 4, 16, 8192, "%.0f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::SliderInt("Detail", &terrainOctaves, 1, 8);
        if (ImGui::Button("Mountains")) {
            doc.pushUndo();
            terrain::mountains(f, terrainMountain, terrainOctaves, terrainSeed++);
            terrain::write(f, doc.ents[te]);
            terrain::rebuildCells(doc, f, 0, 0, f.nx, f.ny);
            log("Terrain " + f.id + ": generated mountains");
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Replaces the heights with fractal hills (a new random set each click)");
        ImGui::SameLine();
        if (ImGui::Button("Flatten all")) {
            doc.pushUndo();
            std::fill(f.h.begin(), f.h.end(), 0.0);
            terrain::write(f, doc.ents[te]);
            terrain::rebuildCells(doc, f, 0, 0, f.nx, f.ny);
        }
        ImGui::SameLine();
        if (ImGui::Button("Use current texture")) {
            doc.pushUndo();
            f.material = currentMaterial;
            terrain::write(f, doc.ents[te]);
            auto& world = doc.ents[0].brushes;  // retexture the tops (rebuild keeps hand-set ones)
            for (Brush& b : world) {
                std::string id;
                int i, j, t;
                if (terrain::parseTag(b, id, i, j, t) && id == f.id && !b.faces.empty()) b.faces[0].material = f.material;
            }
            terrain::rebuildCells(doc, f, 0, 0, f.nx, f.ny);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            doc.pushUndo();
            terrain::removeBrushes(doc, f.id);
            doc.ents.erase(doc.ents.begin() + te);
            terrainEnt = -1;
            terrainSculpt = false;
            doc.touch();
            ImGui::End();
            return;
        }
        drawTerrainPaint(te, f);
    }
    ImGui::SeparatorText("New terrain");
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt2("Cells", terrainNew);
    terrainNew[0] = std::clamp(terrainNew[0], 1, 128);
    terrainNew[1] = std::clamp(terrainNew[1], 1, 128);
    ImGui::SetNextItemWidth(120);
    ImGui::DragFloat("Cell size", &terrainCell, 1, 8, 1024, "%.0f");
    ImGui::SetNextItemWidth(120);
    ImGui::DragFloat("Depth", &terrainDepth, 1, 8, 4096, "%.0f");
    ImGui::TextDisabled("%g x %g units, %d brushes, texture %s", terrainNew[0] * terrainCell, terrainNew[1] * terrainCell,
                        terrainNew[0] * terrainNew[1] * 2, currentMaterial.c_str());
    if (ImGui::Button("Create terrain", {-1, 0})) {
        terrain::Field nf;
        nf.nx = terrainNew[0];
        nf.ny = terrainNew[1];
        nf.cell = terrainCell;
        nf.depth = terrainDepth;
        const std::string cm = currentMaterial;
        nf.material = (cm.find("caulk") != std::string::npos || cm.find("clip") != std::string::npos || cm.empty()) ? "ch_ground_dirt03" : cm;
        // On the selection's floor (its min corner), else centred where the camera looks.
        AABB sel = doc.selectionBounds();
        if (sel.valid()) {
            nf.origin = {sel.mins.x, sel.mins.y, sel.maxs.z};
        } else {
            Vec3 hit;
            Vec3 p = cameraRayHit(camera.forward(), hit) ? hit : camera.pos + camera.forward() * 512.0;
            nf.origin = {snapTo(p.x - nf.nx * nf.cell / 2, gridSize), snapTo(p.y - nf.ny * nf.cell / 2, gridSize), snapTo(p.z, gridSize)};
        }
        doc.pushUndo();
        terrainEnt = terrain::create(doc, nf);
        doc.sel = {Sel{terrainEnt, -1}};
        doc.touchSelection();
        terrainSculpt = true;
        log("Terrain created: " + std::to_string(nf.nx) + " x " + std::to_string(nf.ny) + " cells (" + std::to_string(nf.nx * nf.ny * 2) +
            " brushes); sculpting on (T), hold the left mouse button on it");
    }
    ImGui::End();
}

// Texture painting by slope: an ordered rule list (first match wins) stored on the terrain entity.
// Every edit repaints immediately (materials only, no geometry rebuild); one undo step per edit.
void App::drawTerrainPaint(int te, terrain::Field& f) {
    ImGui::SeparatorText("Paint by slope");
    const terrain::Field before = f;
    bool changed = false, begin = false;
    auto track = [&](bool edited) {
        if (ImGui::IsItemActivated()) begin = true;
        changed |= edited;
    };
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##preset", "Preset...")) {
        for (const terrain::PaintPreset& p : terrain::paintPresets())
            if (ImGui::Selectable(p.name)) {
                f.paint = p.rules;
                if (f.paintJitter <= 0) f.paintJitter = 4;
                begin = changed = true;
            }
        ImGui::EndCombo();
    }
    if (f.paint.empty()) ImGui::TextDisabled("No rules: every cell uses %s. Pick a preset or add a rule.", f.material.c_str());
    int remove = -1, up = -1;
    for (size_t k = 0; k < f.paint.size(); k++) {
        terrain::PaintRule& r = f.paint[k];
        ImGui::PushID(static_cast<int>(k));
        char buf[128];
        std::snprintf(buf, sizeof buf, "%s", r.material.c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 110);
        if (ImGui::InputText("##mat", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) {
            r.material = buf;
            begin = changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Material (Enter to apply). Rules are tried top to bottom; the first match wins.");
        ImGui::SameLine();
        if (ImGui::Button("Use")) {
            r.material = currentMaterial;
            begin = changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Use the current texture (%s)", currentMaterial.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(k == 0);
        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) up = static_cast<int>(k);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("X")) remove = static_cast<int>(k);
        float s0 = static_cast<float>(r.minSlope), s1 = static_cast<float>(r.maxSlope);
        ImGui::SetNextItemWidth(-60);
        track(ImGui::DragFloatRange2("Slope", &s0, &s1, 0.25f, 0, 90, "%.0f deg", "%.0f deg", ImGuiSliderFlags_AlwaysClamp));
        r.minSlope = s0, r.maxSlope = s1;
        bool useHeight = r.minHeight > -1e5 || r.maxHeight < 1e5;
        if (ImGui::Checkbox("Height", &useHeight)) {
            if (useHeight) {
                double lo = 1e30, hi = -1e30;
                for (double h : f.h) lo = std::min(lo, h), hi = std::max(hi, h);
                r.minHeight = lo + (hi - lo) * 0.6, r.maxHeight = 1e6;  // default: the top 40%
            } else {
                r.minHeight = -1e6, r.maxHeight = 1e6;
            }
            begin = changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Also limit this rule to a height band (above the terrain's base), e.g. snow or rock on peaks");
        if (useHeight) {
            ImGui::SameLine();
            float h0 = static_cast<float>(r.minHeight), h1 = static_cast<float>(std::min(r.maxHeight, 99999.0));
            ImGui::SetNextItemWidth(-1);
            track(ImGui::DragFloatRange2("##h", &h0, &h1, 4, static_cast<float>(-f.depth), 99999, "%.0f", h1 >= 99999 ? "top" : "%.0f"));
            r.minHeight = h0, r.maxHeight = h1 >= 99999 ? 1e6 : h1;
        }
        ImGui::PopID();
    }
    if (remove >= 0) f.paint.erase(f.paint.begin() + remove), begin = changed = true;
    if (up > 0) std::swap(f.paint[up], f.paint[up - 1]), begin = changed = true;
    if (ImGui::Button("Add rule")) {
        f.paint.push_back({currentMaterial, f.paint.empty() ? 0.0 : 40.0, 90});
        if (f.paintJitter <= 0) f.paintJitter = 4;
        begin = changed = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A rule painting the current texture on slopes over 40 degrees");
    ImGui::SameLine();
    float jit = static_cast<float>(f.paintJitter);
    ImGui::SetNextItemWidth(-60);
    track(ImGui::SliderFloat("Jitter", &jit, 0, 15, "%.1f deg"));
    f.paintJitter = jit;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Randomises each triangle's slope thresholds so borders between textures are ragged, not straight");
    if (ImGui::Button("Repaint all")) {
        doc.pushUndo();
        const int n = terrain::repaint(doc, f, &before, true);
        log("Terrain " + f.id + ": repainted " + std::to_string(n) + " faces (hand-set textures replaced)");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Repaint every cell from the rules, including cells you retextured by hand.\n"
                          "Edits above repaint automatically but keep hand-set textures, and so does sculpting.");
    if (!f.paint.empty()) {  // coverage readout
        std::map<std::string, int> count;
        for (const Brush& b : doc.ents[0].brushes) {
            std::string id;
            int i, j, t;
            if (terrain::parseTag(b, id, i, j, t) && id == f.id && !b.faces.empty()) count[b.faces[0].material]++;
        }
        const int total = std::max(1, f.nx * f.ny * 2);
        for (const auto& [m, c] : count) ImGui::TextDisabled("  %5.1f%%  %s", 100.0 * c / total, m.c_str());
    }
    if (begin) doc.pushUndo();  // snapshot before this edit (a drag's later frames are part of it)
    if (!changed) return;
    terrain::write(f, doc.ents[te]);
    terrain::repaint(doc, f, &before, false);
}

void App::finishPen() {
    if (penView < 0 || penPoints.size() < 3) return;
    const GridView& v = views[penView];
    double w0 = workMin[v.w], w1 = workMax[v.w];
    if (w1 - w0 < 1) w1 = w0 + 64;
    std::string err;
    std::vector<Brush> made = shapes::fromOutline(penPoints, v.u, v.v, v.w, w0, w1, currentMaterial, err);
    if (made.empty()) {
        log("Pen: " + err + " (fix it with Backspace, or Esc to start over)");
        return;
    }
    doc.pushUndo();
    std::vector<Sel> added;
    for (Brush& b : made) added.push_back(doc.addBrush(std::move(b)));
    doc.sel = added;
    doc.touchSelection();
    log("Pen: " + std::to_string(penPoints.size()) + "-point outline -> " + std::to_string(made.size()) + " brush" +
        (made.size() > 1 ? "es (split into convex pieces)" : "") + ", " + std::to_string(static_cast<int>(w1 - w0)) + " deep");
    penPoints.clear();
}

void App::addShape(int shapeIndex) {
    const shapes::Shape shape = static_cast<shapes::Shape>(shapeIndex);
    AABB box;
    bool fromSelection = false;
    for (const Sel& s : doc.sel)
        if (s.brush >= 0) {
            const AABB& b = doc.ents[s.ent].brushes[s.brush].bounds;
            box.add(b.mins);
            box.add(b.maxs);
            fromSelection = true;
        }
    if (!fromSelection) {
        Vec3 hit;
        Vec3 p = cameraRayHit(camera.forward(), hit) ? hit : camera.pos + camera.forward() * 256.0;
        p = {snapTo(p.x, gridSize), snapTo(p.y, gridSize), snapTo(p.z, gridSize)};
        box.add(p + Vec3{-64, -64, 0});
        box.add(p + Vec3{64, 64, 128});
    }
    std::vector<Brush> made = shapes::make(shape, box.mins, box.maxs, shapeParams, currentMaterial);
    if (made.empty()) {
        log(std::string(shapes::name(shape)) + ": the box is too small for that shape (check Thickness)");
        return;
    }
    doc.pushUndo();
    if (fromSelection) {  // replace the selected brushes
        std::vector<Sel> keep;
        for (const Sel& s : doc.sel)
            if (s.brush < 0) keep.push_back(s);
        std::vector<Sel> brushes;
        for (const Sel& s : doc.sel)
            if (s.brush >= 0) brushes.push_back(s);
        doc.sel = brushes;
        doc.deleteSelected();
    }
    std::vector<Sel> added;
    for (Brush& b : made) added.push_back(doc.addBrush(std::move(b)));
    doc.sel = added;
    doc.touchSelection();
    log(std::string("Added ") + shapes::name(shape) + " (" + std::to_string(made.size()) + " brush" + (made.size() > 1 ? "es" : "") + ")");
}

// A script_model using `model` at `at` (selected, undoable).
void App::placeModel(const std::string& model, const Vec3& at) {
    doc.pushUndo();
    Sel s = doc.addPointEntity("script_model", at);
    doc.ents[s.ent].set("model", model);
    doc.ents[s.ent].set("angles", "0 0 0");
    doc.sel = {s};
    doc.touchSelection();
    currentModel = model;
    log("Placed " + model + " at " + std::to_string(static_cast<int>(at.x)) + " " + std::to_string(static_cast<int>(at.y)) + " " +
        std::to_string(static_cast<int>(at.z)));
}

bool App::cameraRayHit(const Vec3& dir, Vec3& hit) {
    double bestT = 1e30;
    for (const Entity& e : doc.ents)
        for (const Brush& b : e.brushes) {
            double t;
            int face;
            if (rayBrush(b, camera.pos, dir, t, face) && t < bestT) bestT = t;
        }
    if (bestT > 1e29) return false;
    hit = camera.pos + dir * bestT;
    return true;
}

// Models browser: every zone_source/xmodels model as a rendered thumbnail (spins while hovered).
// Click selects it; double-click places it in front of the camera; drag it onto a grid view or the
// camera view to place it there; "Use on selection" swaps the model of the selected entities.
void App::drawModelBrowser() {
    if (ImGuiWindow* tw = ImGui::FindWindowByName("Textures"); tw && tw->DockId)
        ImGui::SetNextWindowDockID(tw->DockId, ImGuiCond_FirstUseEver);  // a tab next to Textures
    if (!ImGui::Begin("Models")) {
        ImGui::End();
        return;
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 150);
    ImGui::InputTextWithHint("##mfilter", "search zone_source models", modelFilter, sizeof modelFilter);
    ImGui::SameLine();
    if (ImGui::Button("Reload")) loadZoneSourcePreview();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    ImGui::SliderFloat("##msize", &modelThumbSize, 64, 256, "%.0f px");
    std::vector<const BrowserModel*> shown;
    for (const BrowserModel& m : browserModels)
        if (containsNoCase(m.name, modelFilter)) shown.push_back(&m);
    if (browserModels.empty()) {
        ImGui::TextWrapped(doc.path.empty() ? "Save the map first: models come from zone_source/xmodels next to the .map."
                                            : "No models in zone_source/xmodels next to this map (*.xmodel_export).");
        ImGui::End();
        return;
    }
    // Actions on the current model.
    const BrowserModel* cur = nullptr;
    for (const BrowserModel& m : browserModels)
        if (m.name == currentModel) cur = &m;
    ImGui::BeginDisabled(!cur);
    if (ImGui::Button("Place at camera") && cur) {
        Vec3 hit;
        Vec3 p = cameraRayHit(camera.forward(), hit) ? hit : camera.pos + camera.forward() * 256.0;
        placeModel(cur->name, {snapTo(p.x, gridSize), snapTo(p.y, gridSize), snapTo(p.z, gridSize)});
    }
    ImGui::SameLine();
    int selectedEnts = 0;
    for (const Sel& s : doc.sel)
        if (s.brush < 0 && s.ent > 0) selectedEnts++;
    ImGui::BeginDisabled(selectedEnts == 0);
    if (ImGui::Button("Use on selection") && cur) {
        doc.pushUndo();
        for (const Sel& s : doc.sel)
            if (s.brush < 0 && s.ent > 0) doc.ents[s.ent].set("model", cur->name);
        doc.touch();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (cur)
        ImGui::TextDisabled("%s  |  %zu tris, %zu bones, %.0f x %.0f x %.0f", cur->name.c_str(), cur->tris, cur->bones, cur->size.x,
                            cur->size.y, cur->size.z);
    else
        ImGui::TextDisabled("%zu models  |  click to select, double-click to place, drag into a view", shown.size());

    if (ImGui::BeginChild("mgrid")) {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float ts = modelThumbSize;
        const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (ts + spacing)));
        const float cell = ts + ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y + 8;
        const int rows = (static_cast<int>(shown.size()) + columns - 1) / columns;
        const float spin = static_cast<float>(std::fmod(ImGui::GetTime() * 90.0, 360.0));
        ImGuiListClipper clip;  // only visible thumbnails render
        clip.Begin(rows, cell);
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                for (int c = 0; c < columns; c++) {
                    const int i = r * columns + c;
                    if (i >= static_cast<int>(shown.size())) break;
                    const BrowserModel& m = *shown[i];
                    auto lm = liveModels.find(live::modelKey(m.name));
                    if (c) ImGui::SameLine();
                    ImGui::BeginGroup();
                    ImGui::PushID(i);
                    const ImVec2 at = ImGui::GetCursorScreenPos();
                    const bool hover = ImGui::IsMouseHoveringRect(at, {at.x + ts + 8, at.y + ts + 8});
                    ID3D11ShaderResourceView* srv =
                        lm != liveModels.end() ? renderer.modelThumbnail(m.name, *lm->second, static_cast<int>(ts), hover ? 35.0f + spin : 35.0f)
                                               : nullptr;
                    const bool active = m.name == currentModel;
                    if (active) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(230, 80, 60, 255));
                    if (ImGui::ImageButton("##m", (ImTextureID)(intptr_t)srv, {ts, ts})) currentModel = m.name;
                    if (active) ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        Vec3 hit;
                        Vec3 p = cameraRayHit(camera.forward(), hit) ? hit : camera.pos + camera.forward() * 256.0;
                        placeModel(m.name, {snapTo(p.x, gridSize), snapTo(p.y, gridSize), snapTo(p.z, gridSize)});
                    }
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("IW4R_XMODEL", m.name.c_str(), m.name.size() + 1);
                        if (srv) ImGui::Image((ImTextureID)(intptr_t)srv, {64, 64});
                        ImGui::Text("%s", m.name.c_str());
                        ImGui::EndDragDropSource();
                    }
                    if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                        std::string texs;
                        for (const std::string& t : m.textures) texs += "\n  " + t;
                        ImGui::SetTooltip("%s\n%zu triangles, %zu vertices, %zu surfaces, %zu bones\nsize %.1f x %.1f x %.1f\ntextures:%s",
                                          m.name.c_str(), m.tris, m.verts, m.surfaces, m.bones, m.size.x, m.size.y, m.size.z, texs.c_str());
                    }
                    ImGui::PushClipRect(ImGui::GetCursorScreenPos(),
                                        {ImGui::GetCursorScreenPos().x + ts + 6, ImGui::GetCursorScreenPos().y + 20}, true);
                    ImGui::TextUnformatted(m.name.c_str());
                    ImGui::PopClipRect();
                    ImGui::PopID();
                    ImGui::EndGroup();
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void App::drawConsole() {
    if (!ImGui::Begin("Console")) {
        ImGui::End();
        return;
    }
    std::lock_guard lock(logMutex);
    for (const std::string& s : logLines) ImGui::TextUnformatted(s.c_str());
    if (logScroll) {
        ImGui::SetScrollHereY(1.0f);
        logScroll = false;
    }
    ImGui::End();
}

void App::drawSettings() {
    if (!ImGui::Begin("Settings", &showSettings, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    char buf[MAX_PATH];
    strncpy_s(buf, gameInput.c_str(), _TRUNCATE);
    ImGui::SetNextItemWidth(520);
    if (ImGui::InputText("Game folder", buf, sizeof buf)) gameInput = buf;
    if (ImGui::Button("Reload assets")) {
        renderer.clearTextures();
        assets.init(gameInput, cacheDir, [this](const std::string& s) { log(s); });
    }
    // Deploy folder for Compile & Run (F5): where mp_<name>.ff is copied for the game to load.
    char deploy[MAX_PATH];
    strncpy_s(deploy, deployDir.c_str(), _TRUNCATE);
    ImGui::SetNextItemWidth(520);
    if (ImGui::InputText("Custom-zone folder (F5 deploy)", deploy, sizeof deploy)) deployDir = deploy;
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        std::string picked = folderDialog(hwnd, "Choose the folder the game loads custom zones from");
        if (!picked.empty()) deployDir = picked;
    }
    if (ImGui::Button("Save launch settings")) saveLaunchConfig();
    ImGui::SliderFloat("Thumbnail size", &thumbSize, 48, 192, "%.0f");
    float speed = static_cast<float>(camSpeed);
    if (ImGui::SliderFloat("Camera speed", &speed, 50, 2000, "%.0f")) camSpeed = speed;
    ImGui::End();
}
