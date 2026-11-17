#pragma once
#include <windows.h>
#include <filesystem>
#include <set>
#include <string>
#include <vector>
#include "assetpanel.h"
#include "renderer.h"

// IW4 Mod Tools: the main program. It lists the maps under maps/ (each maps/<map>/ with its .map,
// script, zone_source and compiled .ff), runs the Asset Manager on the selected map's zone_source,
// and launches Radiant (iw4radiant.exe, a separate program) to edit a map's geometry.
class ModTools : public AssetHost {
public:
    // startMap: the map to select ("" = the last one used, else the first).
    void init(ID3D11Device* device, ID3D11DeviceContext* context, const std::string& startMap);
    void frame();
    bool confirmExit();  // unsaved weapon edits; false = cancel
    void selectMap(const std::string& name);
    void selectAsset(const std::string& name) { assets.select(name); }
    HWND hwnd = nullptr;
    std::string focusWindow;  // brought to the front once the layout exists (--focus)

    // AssetHost
    void log(const std::string& text) override;
    HWND window() override { return hwnd; }
    ID3D11ShaderResourceView* modelThumbnail(const std::string& model, int size, float yawDeg) override;
    ID3D11ShaderResourceView* imageTexture(const std::string& path) override;
    void reloadPreview() override;
    int entitiesUsingModel(const std::string& model) override;
    void renameEntityModels(const std::string& from, const std::string& to) override;

private:
    struct MapEntry {
        std::string name;
        std::filesystem::path dir, map, script, zone, zoneSource;
        uintmax_t mapBytes = 0;
        bool hasScript = false, hasZone = false, hasZoneSource = false;
        std::string mapTime, zoneTime;  // last modified, as text
        size_t sourceFiles = 0;
    };
    void scanMaps();
    const MapEntry* current() const { return selected >= 0 && selected < static_cast<int>(maps.size()) ? &maps[selected] : nullptr; }
    void drawMenu();
    void drawToolbar();
    void drawDockspace();
    void drawMaps();
    void drawConsole();
    void drawNewMapPopup();
    void launchRadiant(const std::string& mapPath);
    void pollRadiants();
    bool radiantHas(const std::filesystem::path& map) const;
    std::string readMapText(std::string& path) const;

    std::string mapsDir;
    std::vector<MapEntry> maps;
    int selected = -1;
    Renderer renderer;
    live::LiveModelLibrary previewModels;
    std::set<std::string> imageKeys;
    AssetPanel assets{*this};
    struct Radiant {
        HANDLE process = nullptr;
        std::filesystem::path map;
    };
    std::vector<Radiant> radiants;
    std::vector<std::string> logLines;
    bool logScroll = false;
    bool newMapOpen = false;
    char newMapName[64] = "";
    std::string newMapError;
    bool firstFrame = true;
};
