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
    void render(const FlyCamera& cam, int width, int height);
    ID3D11ShaderResourceView* output() const { return colorSrv.Get(); }

    // Returns the texture if it's ready, otherwise queues it and returns the placeholder.
    ID3D11ShaderResourceView* texture(const std::string& material);
    void pumpTextures(int budget);  // uploads a few queued textures per frame
    void clearTextures();
    int pendingTextures() const { return static_cast<int>(queue.size()); }
    double aspect() const { return viewHeight ? static_cast<double>(viewWidth) / viewHeight : 1.0; }

    // View toggles (call rebuild after changing showBrushes).
    bool showRefMesh = true;  // imported render geometry (Document::refMesh)
    bool showBrushes = true;
    bool showToolFaces = true;  // caulk/clip/nodraw/trigger... faces in the 3D view
    bool showLighting = true;   // shade with the baked lighting (when there is a bake)

private:
    struct Vertex {
        float pos[3], normal[3], uv[2], color[4];
        float lm[2] = {-1, -1};  // lightmap coord; -1 = unbaked, -2 = sky face
    };
    struct Batch {
        std::string material;
        UINT first = 0, count = 0;
    };
    bool ensureTarget(int width, int height);
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
    ComPtr<ID3D11RasterizerState> rasterFill, rasterLine, rasterRef;
    std::shared_ptr<const RefMesh> refUploaded;
    ComPtr<ID3D11Buffer> refVb, refIb;
    std::vector<RefMesh::Batch> refBatches;
    ComPtr<ID3D11DepthStencilState> depthOn;
    ComPtr<ID3D11ShaderResourceView> white;
    ComPtr<ID3D11SamplerState> lmSampler;
    std::shared_ptr<const light::Bake> lighting;
    ComPtr<ID3D11ShaderResourceView> lmLight, lmSun;  // baked non-sun light (float rgb), sun mask
    float sunDir[3] = {0, 0, 1}, sunColor[3] = {1, 1, 1}, skyColor[3] = {0.55f, 0.68f, 0.85f};

    ComPtr<ID3D11Texture2D> color, depth;
    ComPtr<ID3D11RenderTargetView> colorRtv;
    ComPtr<ID3D11ShaderResourceView> colorSrv;
    ComPtr<ID3D11DepthStencilView> depthDsv;
    int viewWidth = 0, viewHeight = 0;

    ComPtr<ID3D11Buffer> triBuffer, lineBuffer;
    UINT triCapacity = 0, lineCapacity = 0, lineCount = 0;
    std::vector<Batch> batches;

    std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> textures;
    std::unordered_set<std::string> queued;
    std::deque<std::string> queue;
};
