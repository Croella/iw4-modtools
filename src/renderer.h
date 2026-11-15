#pragma once
#include <d3d11.h>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <wrl/client.h>
#include "assets.h"
#include "document.h"
#include "live/liveimport.h"
#include "light/lightbake.h"

using Microsoft::WRL::ComPtr;

struct FlyCamera {
    Vec3 pos{-256, -256, 200};
    double yaw = 45, pitch = -15;  // degrees
    Vec3 forward() const;
    Vec3 right() const;
    Vec3 up() const;
};

// Draws the 3D view into an offscreen target and owns all GPU textures.
class Renderer {
public:
    bool init(ID3D11Device* device, ID3D11DeviceContext* context, std::string& error);
    void setAssets(AssetDb* db) { assets = db; }
    // Models drawn for point entities with a "model" key (call rebuild after the library changes).
    void setModels(const live::LiveModelLibrary* lib) { models = lib; }

    void rebuild(const Document& doc);  // call when doc.revision changes
    // Baked lighting for the preview (nullptr clears it). Call rebuild afterwards.
    void setLighting(std::shared_ptr<const light::Bake> bake);
    bool hasLighting() const { return lighting != nullptr; }
    // Sun shafts + height fog over the lit preview (worldspawn volumetric_* keys, like the game).
    void setVolumetric(const light::Volumetric& v) { vol = v; }
    void render(const FlyCamera& cam, int width, int height);
    ID3D11ShaderResourceView* output() const { return colorSrv.Get(); }

    // Returns the texture if it's ready, otherwise queues it and returns the placeholder.
    ID3D11ShaderResourceView* texture(const std::string& material);
    void pumpTextures(int budget);  // uploads a few queued textures per frame
    void clearTextures();
    // A texture loaded from an image file (png/tga) for a material name, e.g. a zone_source model's.
    void setTextureFile(const std::string& material, const std::string& path);

    // A square preview of a model (textured, the editor's default shading, framed on its bounds
    // from the front-left at `yawDeg`), cached per key. Re-rendered when the yaw changes (hover
    // spin) and until every texture it uses has loaded.
    ID3D11ShaderResourceView* modelThumbnail(const std::string& key, const live::LiveModel& model, int size, float yawDeg);
    void clearThumbnails() { thumbs.clear(); }
    int pendingTextures() const { return static_cast<int>(queue.size()); }
    double aspect() const { return viewHeight ? static_cast<double>(viewWidth) / viewHeight : 1.0; }

    // View toggles (call rebuild after changing showBrushes).
    bool showRefMesh = true;  // imported render geometry (Document::refMesh)
    bool showBrushes = true;
    bool showToolFaces = true;  // caulk/clip/nodraw/trigger... faces in the 3D view
    bool showLighting = true;   // shade with the baked lighting (when there is a bake)
    // Back-face culling like the game: faces seen from behind (e.g. a brush from the inside)
    // aren't drawn. Off by default so rooms can be built from either side.
    bool cullBackFaces = false;
    bool showVolumetrics = true;  // volumetric preview (needs the baked lighting shown)

private:
    struct Vertex {
        float pos[3], normal[3], uv[2], color[4];
        float lm[2] = {-1, -1};  // lightmap coord; -1 = unbaked, -2 = sky face
    };
    // A falloff/additive "effect" material (matdef::Shading::FalloffAdd, e.g. atmos_particles_dust):
    // drawn last, additively, with scrolling uvs and view-angle falloff like the game's shader.
    struct Effect {
        float uvAnim[4] = {}, falloff[4] = {}, beginColor[4] = {1, 1, 1, 1}, endColor[4] = {0, 0, 0, 0};
    };
    struct Batch {
        std::string material;
        UINT first = 0, count = 0;
        bool effect = false;
        Effect fx;
    };
    bool ensureTarget(int width, int height);
    void postProcess(const float viewProj[16], const FlyCamera& cam);
    void uploadVertices(ComPtr<ID3D11Buffer>& buffer, UINT& capacity, const std::vector<Vertex>& data);
    void uploadRefMesh(std::shared_ptr<const RefMesh> mesh);
    ID3D11ShaderResourceView* makeTexture(const IwiImage& image);
    ID3D11ShaderResourceView* makeChecker(const std::string& seed);

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    AssetDb* assets = nullptr;
    const live::LiveModelLibrary* models = nullptr;

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> rasterFill, rasterLine, rasterRef, rasterCull;
    std::shared_ptr<const RefMesh> refUploaded;
    ComPtr<ID3D11Buffer> refVb, refIb;
    std::vector<RefMesh::Batch> refBatches;
    ComPtr<ID3D11DepthStencilState> depthOn, depthTestOnly;
    ComPtr<ID3D11BlendState> blendAdd;
    double startTime = 0;
    ComPtr<ID3D11ShaderResourceView> white;
    ComPtr<ID3D11SamplerState> lmSampler;
    std::shared_ptr<const light::Bake> lighting;
    ComPtr<ID3D11ShaderResourceView> lmLight, lmSun;  // baked non-sun light (float rgb), sun mask
    float sunDir[3] = {0, 0, 1}, sunColor[3] = {1, 1, 1}, skyColor[3] = {0.55f, 0.68f, 0.85f};

    ComPtr<ID3D11Texture2D> color, depth;
    ComPtr<ID3D11RenderTargetView> colorRtv;
    ComPtr<ID3D11ShaderResourceView> colorSrv;
    ComPtr<ID3D11DepthStencilView> depthDsv;
    ComPtr<ID3D11ShaderResourceView> depthSrv;
    // Volumetric preview.
    light::Volumetric vol;
    ComPtr<ID3D11Texture2D> sceneCopy, half[2];
    ComPtr<ID3D11ShaderResourceView> sceneCopySrv, halfSrv[2];
    ComPtr<ID3D11RenderTargetView> halfRtv[2];
    ComPtr<ID3D11VertexShader> postVs;
    ComPtr<ID3D11PixelShader> psFog, psMask, psBlur, psComposite, psOpaque, psSolid;
    ComPtr<ID3D11Buffer> postConstants;
    ComPtr<ID3D11BlendState> blendAlphaOnly, blendScreen;
    ComPtr<ID3D11DepthStencilState> depthOff;
    int viewWidth = 0, viewHeight = 0;

    ComPtr<ID3D11Buffer> triBuffer, lineBuffer;
    UINT triCapacity = 0, lineCapacity = 0, lineCount = 0;
    UINT occFirst = 0, occCount = 0;  // player clip faces in triBuffer: invisible sun occluders for the volumetric preview
    std::vector<Batch> batches;

    struct Thumb {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
        int size = 0;
        float yaw = 0;
        bool complete = false;  // all textures were loaded when it was drawn
    };
    std::unordered_map<std::string, Thumb> thumbs;
    ComPtr<ID3D11Texture2D> thumbDepth;
    ComPtr<ID3D11DepthStencilView> thumbDsv;
    int thumbDepthSize = 0;
    ComPtr<ID3D11Buffer> thumbVb;
    UINT thumbCapacity = 0;

    std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> textures;
    std::unordered_set<std::string> queued;
    std::deque<std::string> queue;
    std::unordered_map<std::string, std::string> textureFiles;  // material -> image file
};
