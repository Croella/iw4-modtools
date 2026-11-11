#include "renderer.h"
#include "zone/gfxworld.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <map>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int kMaxTextureDim = 1024;

const char kShader[] = R"(
// options: x = unlit (lines), y = baked lighting on. Lit like IW4 world surfaces: the baked
// non-sun light plus the sun mask * sun colour * N.L, on top of the texture (shown gamma-encoded).
cbuffer Constants : register(b0) {
    row_major float4x4 viewProj; float4 options; float4 sunDir; float4 sunColor; float4 skyColor;
};
struct VsIn  { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; float4 color : COLOR; float2 lm : TEXCOORD1; };
struct VsOut { float4 pos : SV_Position; float3 normal : NORMAL; float2 uv : TEXCOORD0; float4 color : COLOR; float2 lm : TEXCOORD1; };
Texture2D tex : register(t0);
Texture2D lmLight : register(t1);
Texture2D lmSun : register(t2);
SamplerState samp : register(s0);
SamplerState lmSamp : register(s1);
VsOut vsMain(VsIn i) {
    VsOut o;
    o.pos = mul(viewProj, float4(i.pos, 1));
    o.normal = i.normal;
    o.uv = i.uv;
    o.color = i.color;
    o.lm = i.lm;
    return o;
}
float4 psMain(VsOut i) : SV_Target {
    if (options.x > 0.5) return i.color;
    if (i.lm.x < -1.5) return float4(skyColor.rgb * i.color.rgb, 1);  // sky face
    float3 albedo = tex.Sample(samp, i.uv).rgb * i.color.rgb;
    if (options.y > 0.5 && i.lm.x >= 0) {
        float3 e = lmLight.Sample(lmSamp, i.lm).rgb;
        float vis = lmSun.Sample(lmSamp, i.lm).r;
        float3 l = e + vis * sunColor.rgb * saturate(dot(normalize(i.normal), sunDir.xyz));
        return float4(albedo * sqrt(l), 1);
    }
    float light = 0.72 + 0.28 * dot(i.normal, normalize(float3(0.35, 0.55, 0.76)));
    return float4(albedo * light, 1);
}
)";

struct Constants {
    float viewProj[16];
    float options[4];
    float sunDir[4], sunColor[4], skyColor[4];
};

void multiply(const float a[16], const float b[16], float out[16]) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            out[r * 4 + c] = 0;
            for (int k = 0; k < 4; k++) out[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
        }
}

DXGI_FORMAT dxgiFormat(IwiFormat f) {
    switch (f) {
        case IwiFormat::Dxt1: return DXGI_FORMAT_BC1_UNORM;
        case IwiFormat::Dxt3: return DXGI_FORMAT_BC2_UNORM;
        case IwiFormat::Dxt5: return DXGI_FORMAT_BC3_UNORM;
        default: return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}
}  // namespace

Vec3 FlyCamera::forward() const {
    double y = yaw * kPi / 180, p = pitch * kPi / 180;
    return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
}
Vec3 FlyCamera::right() const { return normalize(cross(forward(), {0, 0, 1})); }
Vec3 FlyCamera::up() const { return cross(right(), forward()); }

bool Renderer::init(ID3D11Device* device, ID3D11DeviceContext* context, std::string& error) {
    dev = device;
    ctx = context;

    ComPtr<ID3DBlob> vsBlob, psBlob, errors;
    HRESULT hr = D3DCompile(kShader, sizeof kShader - 1, "editor", nullptr, nullptr, "vsMain", "vs_5_0", 0, 0, &vsBlob, &errors);
    if (SUCCEEDED(hr))
        hr = D3DCompile(kShader, sizeof kShader - 1, "editor", nullptr, nullptr, "psMain", "ps_5_0", 0, 0, &psBlob, &errors);
    if (FAILED(hr)) {
        error = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "shader compile failed";
        return false;
    }
    dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
    D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    dev->CreateInputLayout(elements, 5, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout);

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Constants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&cb, nullptr, &constants);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.MaxAnisotropy = 8;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &sampler);
    D3D11_SAMPLER_DESC ld{};
    ld.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ld.AddressU = ld.AddressV = ld.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ld.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&ld, &lmSampler);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;  // brushes are viewed from both sides while building rooms
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &rasterFill);
    // Imported reference geometry sits slightly behind, so editable brushes on the same plane win.
    rd.DepthBias = 400;
    rd.SlopeScaledDepthBias = 2.0f;
    dev->CreateRasterizerState(&rd, &rasterRef);
    rd.DepthBias = -200;  // pulls wireframes off the surfaces they outline
    rd.SlopeScaledDepthBias = -1.0f;
    rd.AntialiasedLineEnable = TRUE;
    dev->CreateRasterizerState(&rd, &rasterLine);

    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    dev->CreateDepthStencilState(&ds, &depthOn);

    IwiImage w;
    w.format = IwiFormat::Rgba;
    IwiLevel lvl;
    lvl.width = lvl.height = 1;
    lvl.rowPitch = 4;
    lvl.data = {255, 255, 255, 255};
    w.levels.push_back(std::move(lvl));
    white = makeTexture(w);
    return true;
}

ID3D11ShaderResourceView* Renderer::makeTexture(const IwiImage& image) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = image.levels[0].width;
    td.Height = image.levels[0].height;
    td.MipLevels = static_cast<UINT>(image.levels.size());
    td.ArraySize = 1;
    td.Format = dxgiFormat(image.format);
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> init(image.levels.size());
    for (size_t i = 0; i < image.levels.size(); i++) {
        init[i].pSysMem = image.levels[i].data.data();
        init[i].SysMemPitch = image.levels[i].rowPitch;
    }
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(dev->CreateTexture2D(&td, init.data(), &tex))) return nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    dev->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    return srv;
}

ID3D11ShaderResourceView* Renderer::makeChecker(const std::string& seed) {
    uint32_t h = 2166136261u;
    for (char c : seed) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    uint8_t r = 90 + (h & 0x5f), g = 90 + ((h >> 8) & 0x5f), b = 90 + ((h >> 16) & 0x5f);
    IwiImage img;
    IwiLevel lvl;
    lvl.width = lvl.height = 32;
    lvl.rowPitch = 32 * 4;
    lvl.data.resize(32 * 32 * 4);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            bool on = ((x / 8) ^ (y / 8)) & 1;
            uint8_t* p = &lvl.data[(y * 32 + x) * 4];
            p[0] = on ? b : b / 2;
            p[1] = on ? g : g / 2;
            p[2] = on ? r : r / 2;
            p[3] = 255;
        }
    img.levels.push_back(std::move(lvl));
    return makeTexture(img);
}

ID3D11ShaderResourceView* Renderer::texture(const std::string& material) {
    auto it = textures.find(material);
    if (it != textures.end()) return it->second.Get();
    if (queued.insert(material).second) queue.push_back(material);
    return white.Get();
}

void Renderer::pumpTextures(int budget) {
    while (budget-- > 0 && !queue.empty()) {
        std::string name = std::move(queue.front());
        queue.pop_front();
        queued.erase(name);
        ComPtr<ID3D11ShaderResourceView> srv;
        IwiImage img;
        if (assets && assets->hasGame() && assets->loadMaterialImage(name, kMaxTextureDim, img))
            srv.Attach(makeTexture(img));
        if (!srv) srv.Attach(makeChecker(name));  // visibly missing, but still distinct per material
        textures[name] = srv;
    }
}

void Renderer::clearTextures() {
    textures.clear();
    queued.clear();
    queue.clear();
}

void Renderer::uploadVertices(ComPtr<ID3D11Buffer>& buffer, UINT& capacity, const std::vector<Vertex>& data) {
    if (data.empty()) return;
    if (data.size() > capacity) {
        capacity = static_cast<UINT>(data.size() * 3 / 2 + 1024);
        buffer.Reset();
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = capacity * sizeof(Vertex);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&bd, nullptr, &buffer);
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, data.data(), data.size() * sizeof(Vertex));
        ctx->Unmap(buffer.Get(), 0);
    }
}

void Renderer::setLighting(std::shared_ptr<const light::Bake> bake) {
    lighting = std::move(bake);
    lmLight.Reset();
    lmSun.Reset();
    if (!lighting || lighting->width <= 0) {
        lighting.reset();
        return;
    }
    const light::Bake& b = *lighting;
    auto make = [&](UINT w, UINT h, DXGI_FORMAT fmt, const void* data, UINT pitch, ComPtr<ID3D11ShaderResourceView>& out) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{data, pitch, 0};
        ComPtr<ID3D11Texture2D> t;
        if (SUCCEEDED(dev->CreateTexture2D(&td, &init, &t))) dev->CreateShaderResourceView(t.Get(), nullptr, &out);
    };
    std::vector<float> rgba(static_cast<size_t>(b.width) * b.height * 4, 1.0f);
    for (size_t i = 0, n = static_cast<size_t>(b.width) * b.height; i < n; i++)
        for (int k = 0; k < 3; k++) rgba[i * 4 + k] = b.light[i * 3 + k];
    make(b.width, b.height, DXGI_FORMAT_R32G32B32A32_FLOAT, rgba.data(), b.width * 16, lmLight);
    make(b.width * 2, b.height * 2, DXGI_FORMAT_R8_UNORM, b.sunVis.data(), b.width * 2, lmSun);
    for (int k = 0; k < 3; k++) {
        sunDir[k] = static_cast<float>(b.env.sunDir[k]);
        sunColor[k] = static_cast<float>(b.env.sunColor[k]);
    }
    // Sky faces show the sky tint at a readable brightness.
    double m = std::max({b.env.skyColor.x, b.env.skyColor.y, b.env.skyColor.z, 1e-6});
    for (int k = 0; k < 3; k++) skyColor[k] = static_cast<float>(0.85 * b.env.skyColor[k] / m);
}

void Renderer::rebuild(const Document& doc) {
    std::map<std::string, std::vector<Vertex>> byMaterial;
    std::vector<Vertex> lines;

    auto vertex = [](const Vec3& p, const Vec3& n, float u, float v, float r, float g, float b) {
        return Vertex{{(float)p.x, (float)p.y, (float)p.z}, {(float)n.x, (float)n.y, (float)n.z}, {u, v}, {r, g, b, 1}};
    };
    auto addLine = [&](const Vec3& a, const Vec3& b, float r, float g, float bl) {
        lines.push_back(vertex(a, {}, 0, 0, r, g, bl));
        lines.push_back(vertex(b, {}, 0, 0, r, g, bl));
    };

    if (doc.refMesh.get() != refUploaded.get()) uploadRefMesh(doc.refMesh);

    for (size_t e = 0; e < doc.ents.size(); e++) {
        const Entity& ent = doc.ents[e];
        const EntityClass* cls = findEntityClass(ent.classname());
        for (size_t b = 0; b < ent.brushes.size() && showBrushes; b++) {
            bool selected = doc.isSelected({static_cast<int>(e), static_cast<int>(b)});
            float r = 1.0f, g = selected ? 0.55f : 1.0f, bl = selected ? 0.55f : 1.0f;
            if (!selected && e != 0 && cls) { r = cls->r; g = cls->g; bl = cls->b; }
            for (const Face& f : ent.brushes[b].faces) {
                if (f.winding.size() < 3) continue;
                // With the lighting preview on, sky faces stay visible (drawn as sky).
                const bool sky = light::isSkyMaterial(f.material);
                const bool previewSky = sky && e == 0 && showLighting && lighting;
                if (!showToolFaces && !selected && !previewSky && gfx::isToolMaterial(f.material)) continue;
                auto& out = byMaterial[f.material];
                const light::FaceMap* lm = (e == 0 && lighting && !sky) ? lighting->find(f) : nullptr;
                for (size_t i = 1; i + 1 < f.winding.size(); i++) {
                    for (size_t idx : {size_t(0), i, i + 1}) {
                        float u, v;
                        faceUV(f, f.winding[idx], u, v);
                        Vertex vx = vertex(f.winding[idx], f.normal, u, v, r, g, bl);
                        if (previewSky) {
                            vx.lm[0] = vx.lm[1] = -2;
                        } else if (lm) {
                            lighting->lmapCoord(*lm, f.winding[idx], vx.lm[0], vx.lm[1]);
                        }
                        out.push_back(vx);
                    }
                }
                float lr = selected ? 1.0f : 0.0f;
                for (size_t i = 0; i < f.winding.size(); i++)
                    addLine(f.winding[i], f.winding[(i + 1) % f.winding.size()], lr, 0, 0);
            }
        }
        if (ent.brushes.empty() && e != 0) {
            bool selected = doc.isSelected({static_cast<int>(e), -1});
            // A loaded model for this entity: draw it in place (origin, angles, modelscale).
            const live::LiveModel* model = nullptr;
            if (const std::string* name = models ? ent.find("model") : nullptr; name && !name->empty() && (*name)[0] != '*') {
                auto it = models->find(live::modelKey(*name));  // "*N" names are brush submodels
                if (it != models->end()) model = it->second.get();
            }
            if (model) {
                Vec3 o{};
                ent.origin(o);
                double ang[3] = {0, 0, 0};
                if (const std::string* a = ent.find("angles")) std::sscanf(a->c_str(), "%lf %lf %lf", &ang[0], &ang[1], &ang[2]);
                double scale = 1.0;
                if (const std::string* s = ent.find("modelscale")) scale = std::atof(s->c_str());
                if (scale == 0) scale = 1.0;
                // CoD AnglesToAxis: model x = forward, y = left, z = up.
                double p = ang[0] * kPi / 180, y = ang[1] * kPi / 180, ro = ang[2] * kPi / 180;
                double sp = std::sin(p), cp = std::cos(p), sy = std::sin(y), cy = std::cos(y), sr = std::sin(ro), cr = std::cos(ro);
                Vec3 fwd{cp * cy, cp * sy, -sp};
                Vec3 left{sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, sr * cp};
                Vec3 up{cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp};
                float tint = selected ? 0.55f : 1.0f;
                for (const live::LiveModelSurf& s : model->surfs) {
                    auto& out = byMaterial[s.material];
                    auto at = [&](size_t i, Vec3& wp, Vec3& wn) {
                        Vec3 lp{s.pos[3 * i], s.pos[3 * i + 1], s.pos[3 * i + 2]};
                        Vec3 ln{s.nrm[3 * i], s.nrm[3 * i + 1], s.nrm[3 * i + 2]};
                        wp = o + (fwd * lp.x + left * lp.y + up * lp.z) * scale;
                        wn = fwd * ln.x + left * ln.y + up * ln.z;
                    };
                    for (size_t t = 0; t + 2 < s.idx.size(); t += 3)
                        for (size_t k = 0; k < 3; k++) {
                            size_t vi = s.idx[t + k];
                            if (3 * vi + 2 >= s.pos.size()) continue;
                            Vec3 wp, wn;
                            at(vi, wp, wn);
                            out.push_back(vertex(wp, wn, s.uv[2 * vi], s.uv[2 * vi + 1], 1.0f, tint, tint));
                        }
                }
            }
            AABB box = pointEntityBounds(ent);
            float r = cls ? cls->r : 0.8f, g = cls ? cls->g : 0.2f, bl = cls ? cls->b : 0.8f;
            if (selected) { r = 1; g = 0.2f; bl = 0.2f; }
            Vec3 c[8];
            for (int i = 0; i < 8; i++)
                c[i] = {i & 1 ? box.maxs.x : box.mins.x, i & 2 ? box.maxs.y : box.mins.y, i & 4 ? box.maxs.z : box.mins.z};
            static const int edges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (const auto& ed : edges) addLine(c[ed[0]], c[ed[1]], r, g, bl);
        }
    }
    // Ground grid so an empty map still gives a sense of position and scale.
    for (int i = -32; i <= 32; i++) {
        float shade = (i == 0) ? 0.55f : (i % 8 == 0 ? 0.38f : 0.25f);
        double v = i * 128.0, m = 32 * 128.0;
        addLine({v, -m, 0}, {v, m, 0}, shade, shade, shade);
        addLine({-m, v, 0}, {m, v, 0}, shade, shade, shade);
    }

    std::vector<Vertex> tris;
    batches.clear();
    for (auto& [name, verts] : byMaterial) {
        batches.push_back({name, static_cast<UINT>(tris.size()), static_cast<UINT>(verts.size())});
        tris.insert(tris.end(), verts.begin(), verts.end());
    }
    uploadVertices(triBuffer, triCapacity, tris);
    uploadVertices(lineBuffer, lineCapacity, lines);
    lineCount = static_cast<UINT>(lines.size());
}

void Renderer::uploadRefMesh(std::shared_ptr<const RefMesh> mesh) {
    refUploaded = mesh;
    refVb.Reset();
    refIb.Reset();
    refBatches.clear();
    if (!mesh || mesh->indices.empty()) return;
    const size_t n = mesh->positions.size() / 3;
    std::vector<Vertex> verts(n);
    for (size_t i = 0; i < n; i++) {
        Vertex& v = verts[i];
        std::memcpy(v.pos, &mesh->positions[3 * i], 12);
        std::memcpy(v.normal, &mesh->normals[3 * i], 12);
        std::memcpy(v.uv, &mesh->uvs[2 * i], 8);
        v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1;
    }
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = static_cast<UINT>(verts.size() * sizeof(Vertex));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{verts.data(), 0, 0};
    if (FAILED(dev->CreateBuffer(&bd, &init, &refVb))) return;
    bd.ByteWidth = static_cast<UINT>(mesh->indices.size() * sizeof(uint32_t));
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    init.pSysMem = mesh->indices.data();
    if (FAILED(dev->CreateBuffer(&bd, &init, &refIb))) {
        refVb.Reset();
        return;
    }
    refBatches = mesh->batches;
}

bool Renderer::ensureTarget(int width, int height) {
    if (width == viewWidth && height == viewHeight && color) return true;
    color.Reset(); colorRtv.Reset(); colorSrv.Reset(); depth.Reset(); depthDsv.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &color))) return false;
    dev->CreateRenderTargetView(color.Get(), nullptr, &colorRtv);
    dev->CreateShaderResourceView(color.Get(), nullptr, &colorSrv);
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &depth))) return false;
    dev->CreateDepthStencilView(depth.Get(), nullptr, &depthDsv);
    viewWidth = width;
    viewHeight = height;
    return true;
}

void Renderer::render(const FlyCamera& cam, int width, int height) {
    if (width <= 0 || height <= 0 || !ensureTarget(width, height)) return;

    Vec3 f = cam.forward(), r = cam.right(), u = cam.up();
    float view[16] = {
        (float)r.x, (float)r.y, (float)r.z, (float)-dot(r, cam.pos),
        (float)u.x, (float)u.y, (float)u.z, (float)-dot(u, cam.pos),
        (float)-f.x, (float)-f.y, (float)-f.z, (float)dot(f, cam.pos),
        0, 0, 0, 1};
    double fovY = 70.0 * kPi / 180.0, nearZ = 4.0, farZ = 60000.0;
    float ys = (float)(1.0 / std::tan(fovY * 0.5)), xs = ys / (float)aspect();
    float proj[16] = {
        xs, 0, 0, 0,
        0, ys, 0, 0,
        0, 0, (float)(farZ / (nearZ - farZ)), (float)(nearZ * farZ / (nearZ - farZ)),
        0, 0, -1, 0};

    Constants cb{};
    multiply(proj, view, cb.viewProj);
    for (int k = 0; k < 3; k++) {
        cb.sunDir[k] = sunDir[k];
        cb.sunColor[k] = sunColor[k];
        cb.skyColor[k] = skyColor[k];
    }

    float clear[4] = {0.17f, 0.19f, 0.22f, 1};
    ctx->ClearRenderTargetView(colorRtv.Get(), clear);
    ctx->ClearDepthStencilView(depthDsv.Get(), D3D11_CLEAR_DEPTH, 1, 0);
    ID3D11RenderTargetView* rtv = colorRtv.Get();
    ctx->OMSetRenderTargets(1, &rtv, depthDsv.Get());
    ctx->OMSetDepthStencilState(depthOn.Get(), 0);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    D3D11_VIEWPORT vp{0, 0, (float)width, (float)height, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(layout.Get());
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
    ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
    ctx->PSSetSamplers(1, 1, lmSampler.GetAddressOf());
    const bool lit = showLighting && lmLight && lmSun;
    ID3D11ShaderResourceView* lmViews[2] = {lit ? lmLight.Get() : nullptr, lit ? lmSun.Get() : nullptr};
    ctx->PSSetShaderResources(1, 2, lmViews);

    auto setOptions = [&](float unlit) {
        cb.options[0] = unlit;
        cb.options[1] = lit ? 1.0f : 0.0f;
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, &cb, sizeof cb);
            ctx->Unmap(constants.Get(), 0);
        }
    };

    UINT stride = sizeof(Vertex), offset = 0;
    if (showRefMesh && refVb && refIb) {
        setOptions(0);
        ctx->RSSetState(rasterRef.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetVertexBuffers(0, 1, refVb.GetAddressOf(), &stride, &offset);
        ctx->IASetIndexBuffer(refIb.Get(), DXGI_FORMAT_R32_UINT, 0);
        for (const RefMesh::Batch& b : refBatches) {
            ID3D11ShaderResourceView* srv = texture(b.material);
            ctx->PSSetShaderResources(0, 1, &srv);
            ctx->DrawIndexed(b.indexCount, b.firstIndex, 0);
        }
    }
    if (!batches.empty()) {
        setOptions(0);
        ctx->RSSetState(rasterFill.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetVertexBuffers(0, 1, triBuffer.GetAddressOf(), &stride, &offset);
        for (const Batch& b : batches) {
            ID3D11ShaderResourceView* srv = texture(b.material);
            ctx->PSSetShaderResources(0, 1, &srv);
            ctx->Draw(b.count, b.first);
        }
    }
    if (lineCount) {
        setOptions(1);
        ctx->RSSetState(rasterLine.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        ctx->IASetVertexBuffers(0, 1, lineBuffer.GetAddressOf(), &stride, &offset);
        ctx->Draw(lineCount, 0);
    }
    ID3D11RenderTargetView* none = nullptr;
    ctx->OMSetRenderTargets(1, &none, nullptr);
    ID3D11ShaderResourceView* noSrv[3] = {nullptr, nullptr, nullptr};
    ctx->PSSetShaderResources(0, 3, noSrv);
}
