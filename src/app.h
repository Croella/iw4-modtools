#pragma once
#include <imgui.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <set>
#include <memory>
#include <thread>
#include <mutex>
#include <string>
#include <vector>
#include "assets.h"
#include "assetpanel.h"
#include "modelpreview.h"
#include "document.h"
#include "brushshapes.h"
#include "terrain.h"
#include "renderer.h"
#include "zone/zonebuild.h"
#include "light/lightbake.h"

// One orthographic view. u and v are world axes mapped to screen right and up.
struct GridView {
    const char* title;
    int u, v, w;
    double centerU = 0, centerV = 0;
    double zoom = 0.5;  // pixels per world unit
};

class App : public AssetHost {
public:
    void init(ID3D11Device* device, ID3D11DeviceContext* context, const std::string& gamePath);
    void shutdown();
    void frame();  // builds the UI for one frame
    std::string focusWindow;  // --focus: window/tab brought to the front once the layout exists

    bool confirmDiscard();  // asks about unsaved changes; false means cancel
    bool openMap(const std::string& path);
    bool importFastfileMap(const std::string& path);  // File > Import Map from .ff
    bool importLiveModels();  // File > Import Models from Running Game (reads iw4mp.exe or iw4sp.exe memory)
    void loadEntityModels();  // File > Load Entity Models from Running Game: misc_model etc. by name
    void frameAll();  // camera + grid views fit the whole map
    void buildDemo();
    void selectAll();
    void bakeLightingNow();
    void openAssetManager(const std::string& select);  // --assets: shows it, selects that asset (any kind)  // bakes and waits (command line --bake)

    HWND hwnd = nullptr;
    Renderer renderer;
    Document doc;

    // Screen positions of widgets from the last frame, for the scripted UI test.
    struct ViewRect {
        ImVec2 pos, size;
    };
    ImVec2 gridToScreen(int view, double u, double v) const;
    friend class UiTest;  // the scripted UI test reads tool state (pen points, ...)
    ViewRect cameraRect;
    ImVec2 thumbCenters[2];
    FlyCamera camera;
    double gridSize = 16;

private:
    enum class DragMode { None, Create, Move, Resize };
    struct DragBase {
        Sel sel;
        Brush brush;
        Vec3 origin;
    };

    void log(const std::string& text) override;
    // AssetHost (the shared Asset Manager window, assetpanel.h)
    HWND window() override { return hwnd; }
    ID3D11ShaderResourceView* modelThumbnail(const std::string& model, int size, float yawDeg) override;
    ID3D11ShaderResourceView* imageTexture(const std::string& path) override;
    void reloadPreview() override { loadZoneSourcePreview(); }
    bool isEditor() const override { return true; }
    void placeModel(const std::string& model) override { placeModelAtCamera(model); }
    int selectedEntityCount() override;
    void useModelOnSelection(const std::string& model) override;
    int entitiesUsingModel(const std::string& model) override;
    void renameEntityModels(const std::string& from, const std::string& to) override;
    void drawMenu();
    void drawDockspace();
    void drawGrid(GridView& view, int index);
    void drawCamera();
    void drawInspector();
    void drawTextures();
    void drawModelBrowser();  // "Models": zone_source xmodels as thumbnails; click/drag to place
    void drawToolbar();       // the button bar under the menu: converters + shaped brushes
    void convertTgaToPng();   // zone_source/**/*.tga -> .png next to them
    void convertSkeletons();  // zone_source/xmodels: BO1 / CoD4 skeletons -> IW4 (skeleton::convertForIw4)
    void addShape(int shape); // shapes::Shape, fitted to the selected brushes (replacing them) or at the camera
    shapes::Params shapeParams;
    // Pen tool: click points in a grid view; Enter / clicking the first point extrudes the outline
    // through the work area's depth (convex pieces, see shapes::fromOutline).
    // Terrain (terrain.h): the Terrain panel, sculpting in the camera / top view while terrainSculpt.
    void drawTerrainPanel();
    int activeTerrain();
    void drawTerrainPaint(int te, terrain::Field& f);  // the "Paint by slope" section                         // entity index of the terrain being edited, or -1
    bool terrainRayHit(const Vec3& dir, double& x, double& y);  // camera ray vs the active terrain
    void terrainStrokeAt(double x, double y);    // one sculpt step (starts a stroke on the first)
    void endTerrainStroke();
    bool terrainSculpt = false, terrainStroking = false;
    int terrainEnt = -1;
    terrain::Field terrainField;                 // the active field during a stroke
    terrain::Stroke terrainBrush;
    int terrainNew[2] = {32, 32};
    float terrainCell = 64, terrainDepth = 64, terrainMountain = 768;
    int terrainOctaves = 5;
    unsigned terrainSeed = 1;
    double terrainCursor[2] = {0, 0};
    bool terrainCursorValid = false;
    bool penActive = false;
    int penView = -1;
    std::vector<shapes::Point2> penPoints;
    void finishPen();
    bool deleteTgaAfterConvert = false;
    void placeModel(const std::string& model, const Vec3& at);
    void placeModelAtCamera(const std::string& model);  // where the camera looks (grid-snapped)

    // Asset Manager (View > Asset Manager, F4): the shared window (assetpanel.h) on this map's zone_source.
    AssetPanel assetPanel{*this};
    bool showAssets = false;
    std::set<std::string> imageTextures;  // image files registered with the renderer (imageTexture)
    bool cameraRayHit(const Vec3& dir, Vec3& hit);  // nearest brush along a camera ray
    void drawConsole();
    void drawSettings();
    void handleShortcuts();

    void newMap();
    void openDialog();
    void save(bool forceDialog);
    // F5/F7 and their menu items first ask MP or SP (drawCompileTargetPopup), then run these.
    enum class PendingCompile { None, ToFile, AndRun };
    void requestCompile(PendingCompile what);
    void drawCompileTargetPopup();
    void deployToGameZone(const std::string& mapName, const std::vector<uint8_t>& ff, const std::string& already);
    void compileDialog(MapTarget target);  // File > Compile to .ff (F7): compile, loader-check, write
    void compileAndRun(MapTarget target);  // File > Compile & Run in Game (F5): save, compile, deploy, launch
    bool compileCurrentMap(const std::string& mapName, MapTarget target, std::vector<uint8_t>& ff,
                           std::string& err);
    void dumpDialog();     // File > Dump Fast File: inflate + report + extracted files
    void importDialog();
    void refreshMaterials();
    void pickInGrid(const GridView& view, double u, double v, bool additive);
    void applyDrag(const GridView& view, double u, double v);
    void endDrag();
    void cloneSelection();
    void hollowSelection();
    void deleteSelection();
    void applyMaterial(const std::string& name);

    // Lighting (F9): bakes on a worker thread; the result drives the 3D preview and is reused by
    // compiles while the map's lighting inputs are unchanged.
    void startBake();
    void pollBake();          // picks up a finished bake (called every frame)
    void waitForBake();       // blocks until a running bake finishes
    uint64_t currentSceneHash();
    bool bakeUpToDate();
    void addLightAtCamera();
    void refreshUnlitMaterials();  // fills lightSettings.unlitMaterials from the map's materials
    void editMapScript();          // File > Edit Map Script: <map>.gsc next to the .map
    void loadZoneSourcePreview();  // zone_source/xmodels -> model preview library
    void drawWeaponsMenu();        // Weapons: worldspawn "weaponpack" (SP Special Ops add-on zone)
    std::string deployPackFor(const std::string& mapName, MapTarget target, const std::string& folder);

    void loadLaunchConfig();  // deploy folder for "Compile & Run" (persisted under cacheDir)
    void saveLaunchConfig();

    AssetDb assets;
    std::string gameInput, cacheDir;
    std::string lastCompilePath, lastDumpPath, lastImportPath;
    std::string deployDir;  // where "Compile & Run" copies mp_<name>.ff (the game's custom-zone folder)
    live::LiveModelLibrary liveModels;  // XModels read from the running game, drawn on their entities
    // zone_source models for the Models browser (filled by loadZoneSourcePreview).
    using BrowserModel = modelpreview::ModelInfo;
    std::vector<BrowserModel> browserModels;
    std::string currentModel;
    char modelFilter[64] = "";
    float modelThumbSize = 112;

    GridView views[3] = {{"Top (XY)", 0, 1, 2}, {"Front (XZ)", 0, 2, 1}, {"Side (YZ)", 1, 2, 0}};
    ViewRect gridRects[3];
    double camSpeed = 400;
    Vec3 workMin{0, 0, 0}, workMax{128, 128, 128};
    std::string currentMaterial = "caulk";

    DragMode dragMode = DragMode::None;
    int dragView = -1;
    double dragStartU = 0, dragStartV = 0;
    int resizeU = 0, resizeV = 0;
    AABB dragBox;
    bool dragMoved = false;
    std::vector<DragBase> dragBase;
    Sel createdBrush;
    double contextU = 0, contextV = 0, contextW = 0;

    uint64_t meshRevision = 0;
    // Live effect preview (fxpreview.h): starts after a lighting bake or a compile, View > Effects Preview.
    fxpreview::Preview fxPreview;
    bool showEffects = false;
    uint64_t fxRevision = 0;
    void startFxPreview(const char* why);
    void updateFxPreview();
    uint64_t materialsSeen = 0;
    std::vector<MaterialEntry> materials;
    std::vector<int> filtered;
    char filterText[64] = "";
    bool worldOnly = true;
    bool filterDirty = true;
    float thumbSize = 96;

    std::mutex logMutex;
    std::deque<std::string> logLines;
    bool logScroll = false;
    bool showSettings = false;
    PendingCompile pendingCompile = PendingCompile::None;
    light::Settings lightSettings;
    std::shared_ptr<const light::Bake> bakeResult;  // last finished bake (preview + compile)
    std::shared_ptr<light::Bake> bakeOut;           // the one being baked
    light::Progress bakeProgress;
    std::thread bakeThread;
    std::atomic<bool> bakeFinished{false};
    bool bakeOk = false;
    std::string bakeError;
    std::chrono::steady_clock::time_point bakeStarted;
    uint64_t hashRevision = 0, hashValue = 0;  // currentSceneHash cache
    MapTarget compileTarget = MapTarget::MP;  // last choice, preselected in the popup
    bool firstFrame = true;
    char newKey[64] = "", newValue[128] = "";
};
