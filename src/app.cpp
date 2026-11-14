#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include "app.h"
#include <imgui_internal.h>
#include <algorithm>
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
                       const char* defExt = "map", const char* title = nullptr) {
    char file[MAX_PATH] = "";
    if (!current.empty()) strncpy_s(file, current.c_str(), _TRUNCATE);
    OPENFILENAMEA ofn{};
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
    std::vector<modelbuild::Model> models = loadZoneSourceModels(dir, warnings);
    for (const std::string& w : warnings) log("  zone_source: " + w);
    for (const modelbuild::Model& md : models) {
        auto lm = std::make_shared<live::LiveModel>();
        lm->name = md.name;
        for (const modelbuild::Surface& s : md.surfaces) {
            live::LiveModelSurf ls;
            const modelbuild::Material& mm = md.materials[s.material];
            ls.material = "mc/" + md.name + "_" + mm.name;
            std::string stem = std::filesystem::path(mm.texture).stem().string();
            for (const std::string& cand : {std::filesystem::path(mm.texture).filename().string(), stem + ".png", stem + ".tga"}) {
                std::filesystem::path p = std::filesystem::path(dir) / "images" / cand;
                if (!cand.empty() && std::filesystem::exists(p)) {
                    renderer.setTextureFile(ls.material, p.string());
                    break;
                }
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
    std::string p = fileDialog(hwnd, false, doc.path);
    if (!p.empty()) openMap(p);
}

void App::save(bool forceDialog) {
    std::string path = doc.path;
    if (forceDialog || path.empty()) path = fileDialog(hwnd, true, path);
    if (path.empty()) return;
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
    return true;
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
    refreshMaterials();
    handleShortcuts();
    drawDockspace();
    drawMenu();
    for (int i = 0; i < 3; i++) drawGrid(views[i], i);
    drawCamera();
    drawInspector();
    drawTextures();
    drawConsole();
    if (showSettings) drawSettings();
    drawCompileTargetPopup();
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

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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
