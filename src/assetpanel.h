#pragma once
#include <windows.h>
#include <d3d11.h>
#include <future>
#include <memory>
#include <set>
#include <string>
#include "assetmanager.h"

// What the Asset Manager window needs from the program hosting it (Mod Tools or Radiant).
// Everything editor-specific is optional: Mod Tools has no map open, so it leaves those alone.
class AssetHost {
public:
    virtual ~AssetHost() = default;
    virtual void log(const std::string& text) = 0;
    virtual HWND window() = 0;
    // A rendered preview of a zone_source model / an image file (nullptr = none or not ready).
    virtual ID3D11ShaderResourceView* modelThumbnail(const std::string& model, int size, float yawDeg) = 0;
    virtual ID3D11ShaderResourceView* imageTexture(const std::string& path) = 0;
    virtual void reloadPreview() {}  // zone_source models / images changed on disk
    // Map editing (Radiant only).
    virtual bool isEditor() const { return false; }
    virtual void placeModel(const std::string&) {}
    virtual int selectedEntityCount() { return 0; }
    virtual void useModelOnSelection(const std::string&) {}
    virtual int entitiesUsingModel(const std::string&) { return 0; }
    virtual void renameEntityModels(const std::string&, const std::string&) {}
};

// The Asset Manager window: a zone_source folder by kind with details, cross-references, problems,
// a weapon-file editor and file operations (assetmanager.h does the work).
class AssetPanel {
public:
    explicit AssetPanel(AssetHost& h) : host(h) {}
    // The zone_source folder to manage ("" = none); a change rescans.
    void setRoot(const std::string& zoneSourceDir, const char* noRootMessage = nullptr);
    const std::string& root() const { return rootDir; }
    // Draws the window (p_open: close button, nullptr = none). Call every frame while shown.
    void draw(const char* title, bool* open);
    void rescan();          // index now, analysis on a worker thread
    void poll();            // picks up a finished analysis (call every frame, shown or not)
    void select(const std::string& name);  // any kind; waits for the analysis (command line, tests)
    bool hasUnsavedChanges() const { return weaponDirty; }
    bool saveOrDiscard();   // asks about unsaved weapon edits; false = cancelled

private:
    void afterChange(bool previewToo);
    bool selectAsset(assetman::Kind k, const std::string& name);
    void drawTree();
    void drawDetails();
    void drawProblems();
    void drawWeaponEditor(const assetman::Asset& a);
    void drawPopups();
    void importFiles();
    void deleteAsset(const assetman::Asset& a);

    AssetHost& host;
    std::string rootDir, noRoot = "No zone_source folder.";
    assetman::Index index;
    std::shared_ptr<const assetman::Analysis> analysis;
    std::future<std::shared_ptr<assetman::Analysis>> job;
    bool rescanPending = false;
    assetman::Kind selKind = assetman::Kind::Weapon;
    std::string selName;  // "" = none
    bool showProblems = false, showInfo = false;
    char filter[64] = "";
    assetman::WeaponFile weaponEdit;
    std::string weaponEditName;  // the weapon weaponEdit holds
    bool weaponDirty = false;
    char weaponKeyFilter[64] = "";
    char weaponNewKey[64] = "", weaponNewValue[256] = "";
    enum class Popup { None, Rename, Duplicate, NewWeapon };
    Popup popup = Popup::None;
    char popupName[64] = "";
    bool updateRefs = true;
    std::string popupError;
    int assignWeapon = 0, assignSlot = 1, newGun = -1, newWorld = -1;
};
