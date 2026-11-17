#include "modtools/modtools.h"
#include <shellapi.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include "mapio.h"
#include "modelpreview.h"
#include "zone/zonebuild.h"

namespace fs = std::filesystem;

namespace {

std::string exeDir() {
    char buf[MAX_PATH] = "";
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path().string();
}

// The maps folder: a "maps" folder at or above the exe (the repo's maps/ for build/Release).
std::string findMapsDir() {
    std::error_code ec;
    for (fs::path p = exeDir(); !p.empty(); p = p.parent_path()) {
        if (fs::is_directory(p / "maps", ec)) return (p / "maps").string();
        if (p == p.parent_path()) break;
    }
    return "";
}

std::string timeText(const fs::path& p) {
    std::error_code ec;
    const auto ft = fs::last_write_time(p, ec);
    if (ec) return "";
    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
    const std::time_t t = std::chrono::system_clock::to_time_t(sys);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

std::string lastMapFile() { return (fs::path(exeDir()) / "iw4modtools_last.txt").string(); }

bool validMapName(const std::string& n) {
    if (n.empty() || n.size() > 40) return false;
    for (char c : n)
        if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
}

}  // namespace

void ModTools::init(ID3D11Device* device, ID3D11DeviceContext* context, const std::string& startMap) {
    std::string error;
    if (!renderer.init(device, context, error)) log("Renderer error: " + error);
    renderer.setModels(&previewModels);
    mapsDir = findMapsDir();
    scanMaps();
    log(mapsDir.empty() ? "No maps folder found next to or above " + exeDir() : "Maps folder: " + mapsDir);
    std::string first = startMap;
    if (first.empty()) {
        std::ifstream in(lastMapFile());
        std::getline(in, first);
    }
    if (!first.empty()) selectMap(first);
    if (!current() && !maps.empty()) selectMap(maps[0].name);
}

void ModTools::log(const std::string& text) {
    logLines.push_back(text);
    if (logLines.size() > 2000) logLines.erase(logLines.begin(), logLines.begin() + 500);
    logScroll = true;
}

bool ModTools::confirmExit() { return assets.saveOrDiscard(); }

// ---- maps ----

void ModTools::scanMaps() {
    const std::string keep = current() ? current()->name : "";
    maps.clear();
    selected = -1;
    std::error_code ec;
    if (mapsDir.empty() || !fs::is_directory(mapsDir, ec)) return;
    for (const auto& e : fs::directory_iterator(mapsDir, ec)) {
        if (!e.is_directory(ec)) continue;
        const std::string name = e.path().filename().string();
        MapEntry m;
        m.name = name;
        m.dir = e.path();
        m.map = m.dir / (name + ".map");
        if (!fs::is_regular_file(m.map, ec)) continue;
        m.script = m.dir / (name + ".gsc");
        m.zone = m.dir / (name + ".ff");
        m.zoneSource = m.dir / "zone_source";
        m.mapBytes = fs::file_size(m.map, ec);
        m.hasScript = fs::is_regular_file(m.script, ec);
        m.hasZone = fs::is_regular_file(m.zone, ec);
        m.hasZoneSource = fs::is_directory(m.zoneSource, ec);
        m.mapTime = timeText(m.map);
        if (m.hasZone) m.zoneTime = timeText(m.zone);
        if (m.hasZoneSource)
            for (auto it = fs::recursive_directory_iterator(m.zoneSource, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (it->is_regular_file(ec)) m.sourceFiles++;
            }
        maps.push_back(std::move(m));
    }
    std::sort(maps.begin(), maps.end(), [](const MapEntry& a, const MapEntry& b) { return a.name < b.name; });
    for (size_t i = 0; i < maps.size(); i++)
        if (maps[i].name == keep) selected = static_cast<int>(i);
}

void ModTools::selectMap(const std::string& name) {
    for (size_t i = 0; i < maps.size(); i++)
        if (maps[i].name == name) {
            if (static_cast<int>(i) == selected && !assets.root().empty()) return;
            if (assets.hasUnsavedChanges() && !assets.saveOrDiscard()) return;
            selected = static_cast<int>(i);
            assets.setRoot(maps[i].zoneSource.string());
            reloadPreview();
            std::ofstream(lastMapFile()) << name << "\n";
            return;
        }
}

void ModTools::reloadPreview() {
    previewModels.clear();
    renderer.clearThumbnails();
    if (const MapEntry* m = current())
        modelpreview::load(m->zoneSource.string(), renderer, previewModels, nullptr, [this](const std::string& t) { log(t); });
}

ID3D11ShaderResourceView* ModTools::modelThumbnail(const std::string& model, int size, float yawDeg) {
    auto lm = previewModels.find(live::modelKey(model));
    return lm == previewModels.end() ? nullptr : renderer.modelThumbnail(model, *lm->second, size, yawDeg);
}

ID3D11ShaderResourceView* ModTools::imageTexture(const std::string& path) {
    const std::string key = "file:" + path;
    if (imageKeys.insert(key).second) renderer.setTextureFile(key, path);
    return renderer.texture(key);
}

std::string ModTools::readMapText(std::string& path) const {
    const MapEntry* m = current();
    if (!m) return "";
    path = m->map.string();
    std::ifstream in(m->map, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Entities in the selected map with "model" "<model>" (read from the .map text).
int ModTools::entitiesUsingModel(const std::string& model) {
    std::string path;
    std::string text = readMapText(path);
    const std::string needle = "\"model\" \"" + zsrc::lower(model) + "\"";
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    int n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) n++;
    return n;
}

void ModTools::renameEntityModels(const std::string& from, const std::string& to) {
    std::string path;
    std::string text = readMapText(path);
    if (text.empty()) return;
    std::string lowerText = text;
    for (char& c : lowerText) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string needle = "\"model\" \"" + zsrc::lower(from) + "\"", repl = "\"model\" \"" + to + "\"";
    std::string out;
    size_t pos = 0, n = 0;
    for (size_t at = lowerText.find(needle); at != std::string::npos; at = lowerText.find(needle, at + needle.size())) {
        out += text.substr(pos, at - pos) + repl;
        pos = at + needle.size();
        n++;
    }
    if (!n) return;
    out += text.substr(pos);
    std::ofstream(path, std::ios::binary) << out;
    log("  " + std::to_string(n) + " entit" + (n == 1 ? "y" : "ies") + " in " + fs::path(path).filename().string() + " now use " + to);
    if (radiantHas(path))
        MessageBoxA(hwnd,
                    ("Radiant has " + fs::path(path).filename().string() +
                     " open. Reopen the map there before saving, or it will write the old model name back.")
                        .c_str(),
                    "Mod Tools", MB_OK | MB_ICONINFORMATION);
}

// ---- Radiant ----

void ModTools::launchRadiant(const std::string& mapPath) {
    const std::string exe = (fs::path(exeDir()) / "iw4radiant.exe").string();
    std::error_code ec;
    if (!fs::is_regular_file(exe, ec)) {
        MessageBoxA(hwnd, ("Radiant isn't next to Mod Tools:\n" + exe).c_str(), "Mod Tools", MB_OK | MB_ICONERROR);
        return;
    }
    std::string cmd = "\"" + exe + "\"";
    if (!mapPath.empty()) cmd += " \"" + mapPath + "\"";
    STARTUPINFOA si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::vector<char> line(cmd.begin(), cmd.end());
    line.push_back(0);
    const std::string dir = exeDir();
    if (!CreateProcessA(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        log("Couldn't start Radiant (error " + std::to_string(GetLastError()) + ")");
        return;
    }
    CloseHandle(pi.hThread);
    radiants.push_back({pi.hProcess, mapPath});
    log("Started Radiant" + (mapPath.empty() ? std::string() : " on " + mapPath));
}

void ModTools::pollRadiants() {
    for (size_t i = 0; i < radiants.size();) {
        if (WaitForSingleObject(radiants[i].process, 0) == WAIT_OBJECT_0) {
            CloseHandle(radiants[i].process);
            log("Radiant closed" + (radiants[i].map.empty() ? std::string() : " (" + radiants[i].map.filename().string() + ")"));
            radiants.erase(radiants.begin() + static_cast<long>(i));
            scanMaps();  // it may have saved / compiled
            assets.rescan();
            reloadPreview();
        } else {
            i++;
        }
    }
}

bool ModTools::radiantHas(const fs::path& map) const {
    std::error_code ec;
    for (const Radiant& r : radiants)
        if (!r.map.empty() && fs::equivalent(r.map, map, ec)) return true;
    return false;
}

// ---- UI ----

void ModTools::frame() {
    renderer.pumpTextures(8);
    pollRadiants();
    assets.poll();
    drawMenu();
    drawToolbar();
    drawDockspace();
    drawMaps();
    const MapEntry* m = current();
    const std::string title = "Asset Manager###assets";
    assets.draw((m ? "Asset Manager - " + m->name + "###assets" : title).c_str(), nullptr);
    drawConsole();
    drawNewMapPopup();
    if (!focusWindow.empty() && !firstFrame) {
        ImGui::SetWindowFocus(focusWindow.c_str());
        focusWindow.clear();
    }
    firstFrame = false;
}

void ModTools::drawMenu() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Map...")) newMapOpen = true;
        if (ImGui::MenuItem("Refresh Maps")) scanMaps();
        if (ImGui::MenuItem("Open Maps Folder", nullptr, false, !mapsDir.empty()))
            ShellExecuteA(hwnd, "open", mapsDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) PostMessageA(hwnd, WM_CLOSE, 0, 0);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        const MapEntry* m = current();
        if (ImGui::MenuItem("Radiant (selected map)", nullptr, false, m != nullptr)) launchRadiant(m->map.string());
        if (ImGui::MenuItem("Radiant (empty)")) launchRadiant("");
        if (ImGui::MenuItem("Asset Manager")) ImGui::SetWindowFocus("###assets");
        ImGui::EndMenu();
    }
    const MapEntry* m = current();
    const std::string status = (m ? "map: " + m->name : std::string("no map")) + "   |   Radiant: " +
                               (radiants.empty() ? std::string("not running") : std::to_string(radiants.size()) + " open");
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(status.c_str()).x - 16);
    ImGui::TextDisabled("%s", status.c_str());
    ImGui::EndMainMenuBar();
}

void ModTools::drawToolbar() {
    const float h = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2;
    if (ImGui::BeginViewportSideBar("##modtools_toolbar", ImGui::GetMainViewport(), ImGuiDir_Up, h,
                                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        const MapEntry* m = current();
        ImGui::BeginDisabled(!m);
        if (ImGui::Button("Open in Radiant") && m) launchRadiant(m->map.string());
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Edit the selected map's geometry, entities and lighting in Radiant (a separate window).");
        ImGui::SameLine();
        if (ImGui::Button("Asset Manager")) ImGui::SetWindowFocus("###assets");
        ImGui::SameLine();
        if (ImGui::Button("New Map...")) newMapOpen = true;
        ImGui::SameLine();
        ImGui::BeginDisabled(!m);
        if (ImGui::Button("Open Folder") && m) ShellExecuteA(hwnd, "open", m->dir.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SameLine();
        if (ImGui::Button("Edit Script") && m) {
            if (!m->hasScript) std::ofstream(m->script) << "main()\n{\n}\n";
            if (reinterpret_cast<INT_PTR>(ShellExecuteA(hwnd, "open", m->script.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                ShellExecuteA(hwnd, "open", "notepad.exe", ("\"" + m->script.string() + "\"").c_str(), nullptr, SW_SHOWNORMAL);
            scanMaps();
        }
        ImGui::EndDisabled();
    }
    ImGui::End();
}

void ModTools::drawDockspace() {
    ImGuiID id = ImGui::GetID("ModToolsDock");
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockSpaceOverViewport(id, vp);
    if (!firstFrame || ImGui::DockBuilderGetNode(id)->IsSplitNode()) return;
    ImGui::DockBuilderRemoveNode(id);
    ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(id, vp->WorkSize);
    ImGuiID left, rest, bottom, main;
    ImGui::DockBuilderSplitNode(id, ImGuiDir_Left, 0.22f, &left, &rest);
    ImGui::DockBuilderSplitNode(rest, ImGuiDir_Down, 0.18f, &bottom, &main);
    ImGui::DockBuilderDockWindow("Maps", left);
    ImGui::DockBuilderDockWindow("###assets", main);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderFinish(id);
}

void ModTools::drawMaps() {
    if (!ImGui::Begin("Maps")) {
        ImGui::End();
        return;
    }
    if (mapsDir.empty()) {
        ImGui::TextWrapped("No maps folder: Mod Tools looks for a \"maps\" folder next to or above %s.", exeDir().c_str());
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("%s", mapsDir.c_str());
    if (ImGui::BeginChild("##maplist", {0, ImGui::GetContentRegionAvail().y * 0.45f}, ImGuiChildFlags_Borders)) {
        for (size_t i = 0; i < maps.size(); i++) {
            const MapEntry& m = maps[i];
            const bool open = radiantHas(m.map);
            const std::string label = m.name + (open ? "   (in Radiant)" : "");
            if (ImGui::Selectable(label.c_str(), static_cast<int>(i) == selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                selectMap(m.name);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) launchRadiant(m.map.string());
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Double-click to open it in Radiant");
        }
        if (maps.empty()) ImGui::TextDisabled("No maps yet: File > New Map.");
    }
    ImGui::EndChild();
    if (const MapEntry* m = current()) {
        ImGui::SeparatorText(m->name.c_str());
        auto row = [](const char* label, const std::string& value, bool ok = true) {
            ImGui::TextDisabled("%s", label);
            ImGui::SameLine(110);
            if (ok) ImGui::TextUnformatted(value.c_str());
            else ImGui::TextColored({1.0f, 0.78f, 0.30f, 1}, "%s", value.c_str());
        };
        row("Map", m->map.filename().string() + "  (" + std::to_string(m->mapBytes / 1024) + " KB, " + m->mapTime + ")");
        row("Script", m->hasScript ? m->script.filename().string() : "none", m->hasScript);
        row("Compiled", m->hasZone ? m->zone.filename().string() + "  (" + m->zoneTime + ")" : "not yet (compile in Radiant: F7 / F5)", m->hasZone);
        row("zone_source", m->hasZoneSource ? std::to_string(m->sourceFiles) + " files" : "none yet (Import in the Asset Manager)",
            m->hasZoneSource);
        ImGui::Spacing();
        if (ImGui::Button("Open in Radiant", {-1, 0})) launchRadiant(m->map.string());
        if (ImGui::Button("Refresh", {-1, 0})) {
            scanMaps();
            assets.rescan();
            reloadPreview();
        }
    }
    ImGui::End();
}

void ModTools::drawConsole() {
    if (!ImGui::Begin("Console")) {
        ImGui::End();
        return;
    }
    for (const std::string& s : logLines) ImGui::TextUnformatted(s.c_str());
    if (logScroll) {
        ImGui::SetScrollHereY(1.0f);
        logScroll = false;
    }
    ImGui::End();
}

void ModTools::drawNewMapPopup() {
    if (newMapOpen) {
        ImGui::OpenPopup("New map");
        newMapOpen = false;
        newMapName[0] = 0;
        newMapError.clear();
    }
    if (!ImGui::BeginPopupModal("New map", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("Name", newMapName, sizeof newMapName, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::TextDisabled("lowercase letters, digits and _ (e.g. mp_dockyard, zm_bunker)");
    ImGui::TextDisabled("Creates maps/<name>/ with <name>.map, <name>.gsc and an empty zone_source/.");
    if (!newMapError.empty()) ImGui::TextColored({1.0f, 0.38f, 0.33f, 1}, "%s", newMapError.c_str());
    bool ok = ImGui::Button("Create", {100, 0}) || enter;
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {100, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
        ok = false;
    }
    if (ok) {
        const std::string name = newMapName;
        std::error_code ec;
        const fs::path dir = fs::path(mapsDir) / name;
        if (mapsDir.empty()) newMapError = "no maps folder";
        else if (!validMapName(name)) newMapError = "use lowercase letters, digits and _";
        else if (fs::exists(dir, ec)) newMapError = "maps/" + name + " already exists";
        else {
            fs::create_directories(dir / "zone_source", ec);
            for (const char* sub : {"weapons", "xmodels", "xanims", "images"}) fs::create_directories(dir / "zone_source" / sub, ec);
            Document doc;
            doc.reset();
            std::string err;
            if (!saveMap((dir / (name + ".map")).string(), doc, err)) newMapError = err;
            else {
                std::ofstream(dir / (name + ".gsc")) << "main()\n{\n}\n";
                log("Created map " + (dir / (name + ".map")).string());
                scanMaps();
                selectMap(name);
                ImGui::CloseCurrentPopup();
            }
        }
    }
    ImGui::EndPopup();
}
