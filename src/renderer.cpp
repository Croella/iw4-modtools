#include "renderer.h"
#include "zone/gfxworld.h"
#include "model/imageio.h"
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
// options.z = effect material (falloff + uv anim, additive), options.w = time in seconds.
cbuffer Constants : register(b0) {
    row_major float4x4 viewProj; float4 options; float4 sunDir; float4 sunColor; float4 skyColor;
    float4 eyePos; float4 uvAnim; float4 falloff; float4 falloffBegin; float4 falloffEnd;
};
struct VsIn  { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; float4 color : COLOR; float2 lm : TEXCOORD1; };
struct VsOut { float4 pos : SV_Position; float3 normal : NORMAL; float2 uv : TEXCOORD0; float4 color : COLOR; float2 lm : TEXCOORD1; float3 world : TEXCOORD2; };
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
    o.world = i.pos;
    if (options.z > 0.5) {  // uvAnimParms: rotate about the uv centre, then scroll
        float s, c;
        sincos(uvAnim.z * options.w, s, c);
        float2 d = i.uv - 0.5;
        o.uv = float2(c * d.x - s * d.y, s * d.x + c * d.y) + 0.5 + uvAnim.xy * options.w;
    }
    o.color = i.color;
    o.lm = i.lm;
    return o;
}
float4 psMain(VsOut i) : SV_Target {
    if (options.x > 0.5) return i.color;
    if (options.z > 2.5) {  // effect sprite: 3 = additive, 4 = alpha blend (premultiplied)
        float4 t = tex.Sample(samp, i.uv);
        float a = t.a * i.color.a;
        if (options.z < 3.5) return float4(t.rgb * i.color.rgb * a, 0);
        return float4(t.rgb * i.color.rgb * a, a);
    }
    if (i.lm.x < -1.5) return float4(skyColor.rgb * i.color.rgb, 0);  // sky face (alpha 0 = sky for the post passes)
    if (options.z > 0.5) {  // effect: additive, faded by view angle (falloffParms.zw on cos^2)
        float4 t = tex.Sample(samp, i.uv);
        float k = dot(normalize(i.world - eyePos.xyz), normalize(i.normal));
        float f = saturate(k * k * falloff.z + falloff.w);
        float3 c = f * lerp(falloffEnd.rgb, falloffBegin.rgb, f);
        return float4(sqrt(t.rgb * t.rgb * c * t.a), 1);
    }
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

// Volumetric preview (same model as iw4r_mapload.dll's in-game pass): height fog from the depth
// buffer, then screen-space sun shafts (sky mask -> two radial blurs -> added on). Sky = alpha 0.
const char kPostShader[] = R"(
cbuffer Post : register(b0) {
    row_major float4x4 invViewProj;
    float4 eye;       // xyz camera, w fog start
    float4 sun;       // xyz unit vector to the sun, w anisotropy g
    float4 fogColor;  // rgb ambient fog colour, w max opacity
    float4 fogLight;  // rgb sun scattering colour, w sky multiplier
    float4 fogParm;   // x density, y height falloff, z base height, w sky distance
    float4 sunUv;     // xy sun uv, z aspect, w glow radius
    float4 mask;      // x brightness threshold, y sky weight
    float4 glow;      // rgb glow colour * strength
    float4 blur;      // x step, y decay, z weight
    float4 tint;      // rgb colour * intensity, w fade
    float4 flags;     // x debug view
};
Texture2D src : register(t0);
Texture2D<float> depthTex : register(t1);
SamplerState lin : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vsFull(uint id : SV_VertexID) {
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 psFog(V i) : SV_Target {
    float4 c = src.Sample(lin, i.uv);
    float d = depthTex.Load(int3(i.pos.xy, 0));
    float2 ndc = float2(i.uv.x * 2 - 1, 1 - i.uv.y * 2);
    float4 f4 = mul(invViewProj, float4(ndc, 1, 1));
    float3 dir = normalize(f4.xyz / f4.w - eye.xyz);
    bool sky = c.a < 0.5 || d >= 1;
    float L = fogParm.w;
    if (!sky) {
        float4 w4 = mul(invViewProj, float4(ndc, d, 1));
        L = length(w4.xyz / w4.w - eye.xyz);
    }
    float Lf = max(L - eye.w, 0);
    float b = fogParm.y;
    float k = b * dir.z * Lf;
    float shape = abs(k) > 1e-4 ? (1 - exp(-k)) / k : 1;
    float od = fogParm.x * exp(-b * (eye.z - fogParm.z)) * Lf * shape;
    float a = min(1 - exp(-od), fogColor.w) * (sky ? fogLight.w : 1);
    float g = sun.w;
    float hg = (1 - g * g) / pow(max(1 + g * g - 2 * g * dot(dir, sun.xyz), 1e-4), 1.5);
    float3 light = fogColor.rgb + fogLight.rgb * hg;
    if (flags.x > 2.5) return float4(a, a, a, c.a);
    return float4(lerp(c.rgb, light, a), c.a);
}
float4 psMask(V i) : SV_Target {
    float4 c = src.Sample(lin, i.uv);
    float sky = c.a < 0.5 ? 1 : 0;
    float2 dd = (i.uv - sunUv.xy) * float2(sunUv.z, 1);
    float gl = exp(-dot(dd, dd) / (sunUv.w * sunUv.w));
    return float4(sky * (max(c.rgb - mask.x, 0) * mask.y + gl * glow.rgb), 1);
}
#define TAPS 32
float4 psBlur(V i) : SV_Target {
    float2 delta = (i.uv - sunUv.xy) * (blur.x / TAPS);
    float3 sum = 0;
    float illum = 1, wsum = 0;
    float2 p = i.uv;
    [unroll] for (int t = 0; t < TAPS; t++) {
        sum += src.Sample(lin, p).rgb * illum;
        wsum += illum;
        illum *= blur.y;
        p -= delta;
    }
    return float4(sum * blur.z / wsum, 1);
}
float4 psComposite(V i) : SV_Target { return float4(src.Sample(lin, i.uv).rgb * tint.rgb * tint.w, 0); }
float4 psOpaque(V i) : SV_Target { return float4(0, 0, 0, 1); }
float4 psSolid() : SV_Target { return float4(0, 0, 0, 1); }  // any vertex shader
)";

struct PostConstants {
    float invViewProj[16];
    float eye[4], sun[4], fogColor[4], fogLight[4], fogParm[4], sunUv[4], mask[4], glow[4], blur[4], tint[4], flags[4];
};

// Inverse of a row-major 4x4 (cofactors, in double).
bool invert4(const float m[16], float out[16]) {
    double a[16], inv[16];
    for (int i = 0; i < 16; i++) a[i] = m[i];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (det == 0) return false;
    for (int i = 0; i < 16; i++) out[i] = static_cast<float>(inv[i] / det);
    return true;
}

struct Constants {
    float viewProj[16];
    float options[4];
    float sunDir[4], sunColor[4], skyColor[4];
    float eyePos[4], uvAnim[4], falloff[4], falloffBegin[4], falloffEnd[4];
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
    rd.CullMode = D3D11_CULL_BACK;  // brush windings are clockwise seen from outside = D3D front
    dev->CreateRasterizerState(&rd, &rasterCull);
    rd.CullMode = D3D11_CULL_NONE;
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
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;  // effects test depth but don't occlude
    dev->CreateDepthStencilState(&ds, &depthTestOnly);
    D3D11_BLEND_DESC bdesc{};
    bdesc.RenderTarget[0].BlendEnable = TRUE;
    bdesc.RenderTarget[0].SrcBlend = bdesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bdesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bdesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bdesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bdesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bdesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bdesc, &blendAdd);
    {  // effect sprites: colour only (the alpha channel marks the sky for the volumetric passes)
        D3D11_BLEND_DESC spd{};
        D3D11_RENDER_TARGET_BLEND_DESC& t = spd.RenderTarget[0];
        t.BlendEnable = TRUE;
        t.SrcBlend = D3D11_BLEND_ONE;
        t.DestBlend = D3D11_BLEND_ONE;
        t.BlendOp = t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        t.SrcBlendAlpha = D3D11_BLEND_ZERO;
        t.DestBlendAlpha = D3D11_BLEND_ONE;
        t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        dev->CreateBlendState(&spd, &blendSpriteAdd);
        t.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        dev->CreateBlendState(&spd, &blendSpriteBlend);
    }
    startTime = static_cast<double>(GetTickCount64()) / 1000.0;

    // Volumetric preview passes.
    auto compilePost = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& blob) {
        ComPtr<ID3DBlob> errs;
        HRESULT h = D3DCompile(kPostShader, sizeof kPostShader - 1, "post", nullptr, nullptr, entry, target, 0, 0, &blob, &errs);
        if (FAILED(h)) error = errs ? static_cast<const char*>(errs->GetBufferPointer()) : "post shader compile failed";
        return SUCCEEDED(h);
    };
    {
        ComPtr<ID3DBlob> b;
        if (!compilePost("vsFull", "vs_5_0", b)) return false;
        dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &postVs);
        struct P {
            const char* entry;
            ComPtr<ID3D11PixelShader>* out;
        };
        for (P q : {P{"psFog", &psFog}, P{"psMask", &psMask}, P{"psBlur", &psBlur}, P{"psComposite", &psComposite}, P{"psOpaque", &psOpaque}, P{"psSolid", &psSolid}}) {
            ComPtr<ID3DBlob> pb;
            if (!compilePost(q.entry, "ps_5_0", pb)) return false;
            dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, q.out->GetAddressOf());
        }
        D3D11_BUFFER_DESC pcb{};
        pcb.ByteWidth = sizeof(PostConstants);
        pcb.Usage = D3D11_USAGE_DYNAMIC;
        pcb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        pcb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&pcb, nullptr, &postConstants);
        D3D11_BLEND_DESC ad{};
        ad.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
        dev->CreateBlendState(&ad, &blendAlphaOnly);
        // Screen blend for the shafts (c + s * (1 - c)): brightens without blowing out to white.
        D3D11_BLEND_DESC sb{};
        sb.RenderTarget[0].BlendEnable = TRUE;
        sb.RenderTarget[0].SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
        sb.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        sb.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        sb.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        sb.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        sb.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        sb.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev->CreateBlendState(&sb, &blendScreen);
        D3D11_DEPTH_STENCIL_DESC nd{};
        nd.DepthEnable = FALSE;
        dev->CreateDepthStencilState(&nd, &depthOff);
    }

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

void Renderer::setTextureFile(const std::string& material, const std::string& path) {
    textureFiles[material] = path;
    textures.erase(material);  // reload on next use
}

void Renderer::setTextureImage(const std::string& material, const IwiImage& image) {
    ComPtr<ID3D11ShaderResourceView> srv;
    srv.Attach(makeTexture(image));
    if (srv) textures[material] = srv;
}

void Renderer::drawSprites(void* constantsData) {
    Constants& cb = *static_cast<Constants*>(constantsData);
    std::vector<const fxpreview::Sprite*> order;
    order.reserve(sprites.size());
    for (const fxpreview::Sprite& s : sprites) order.push_back(&s);
    std::stable_sort(order.begin(), order.end(), [](const fxpreview::Sprite* a, const fxpreview::Sprite* b) { return a->depth > b->depth; });
    std::vector<Vertex> verts;
    verts.reserve(order.size() * 6);
    for (const fxpreview::Sprite* s : order) {
        const Vec3 c[4] = {s->pos - s->right + s->up, s->pos + s->right + s->up, s->pos + s->right - s->up, s->pos - s->right - s->up};
        const float uv[4][2] = {{s->uv[0], s->uv[1]}, {s->uv[2], s->uv[1]}, {s->uv[2], s->uv[3]}, {s->uv[0], s->uv[3]}};
        for (int k : {0, 1, 2, 0, 2, 3}) {
            Vertex v{};
            v.pos[0] = static_cast<float>(c[k].x), v.pos[1] = static_cast<float>(c[k].y), v.pos[2] = static_cast<float>(c[k].z);
            v.normal[2] = 1;
            v.uv[0] = uv[k][0], v.uv[1] = uv[k][1];
            std::memcpy(v.color, s->color, 16);
            verts.push_back(v);
        }
    }
    uploadVertices(spriteBuffer, spriteCapacity, verts);
    UINT stride = sizeof(Vertex), offset = 0;
    ctx->RSSetState(rasterFill.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetVertexBuffers(0, 1, spriteBuffer.GetAddressOf(), &stride, &offset);
    ctx->OMSetDepthStencilState(depthTestOnly.Get(), 0);
    cb.options[0] = 0;
    cb.options[1] = 0;
    float mode = 0;
    for (size_t i = 0; i < order.size();) {  // runs of the same material and blend mode
        size_t j = i + 1;
        while (j < order.size() && order[j]->material == order[i]->material && order[j]->additive == order[i]->additive) j++;
        const float m = order[i]->additive ? 3.0f : 4.0f;
        if (m != mode) {
            mode = m;
            cb.options[2] = m;
            D3D11_MAPPED_SUBRESOURCE mp;
            if (SUCCEEDED(ctx->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) {
                std::memcpy(mp.pData, &cb, sizeof cb);
                ctx->Unmap(constants.Get(), 0);
            }
            ctx->OMSetBlendState(order[i]->additive ? blendSpriteAdd.Get() : blendSpriteBlend.Get(), nullptr, 0xffffffff);
        }
        ID3D11ShaderResourceView* srv = texture(order[i]->material);
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->Draw(static_cast<UINT>((j - i) * 6), static_cast<UINT>(i * 6));
        i = j;
    }
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(depthOn.Get(), 0);
}

void Renderer::pumpTextures(int budget) {
    while (budget-- > 0 && !queue.empty()) {
        std::string name = std::move(queue.front());
        queue.pop_front();
        queued.erase(name);
        ComPtr<ID3D11ShaderResourceView> srv;
        IwiImage img;
        if (auto f = textureFiles.find(name); f != textureFiles.end()) {
            imageio::Rgba rgba;
            std::string err;
            if (imageio::load(f->second, rgba, err)) {
                IwiLevel lvl;
                lvl.width = rgba.width;
                lvl.height = rgba.height;
                lvl.rowPitch = rgba.width * 4;
                lvl.data.resize(rgba.pixels.size());
                for (size_t i = 0; i + 3 < rgba.pixels.size(); i += 4) {  // RGBA -> BGRA
                    lvl.data[i] = rgba.pixels[i + 2];
                    lvl.data[i + 1] = rgba.pixels[i + 1];
                    lvl.data[i + 2] = rgba.pixels[i];
                    lvl.data[i + 3] = rgba.pixels[i + 3];
                }
                img.format = IwiFormat::Rgba;
                img.levels.push_back(std::move(lvl));
                srv.Attach(makeTexture(img));
            }
        } else if (assets && assets->hasGame() && assets->loadMaterialImage(name, kMaxTextureDim, img)) {
            srv.Attach(makeTexture(img));
        }
        if (!srv) srv.Attach(makeChecker(name));  // visibly missing, but still distinct per material
        textures[name] = srv;
    }
}

void Renderer::clearTextures() {
    textures.clear();
    queued.clear();
    queue.clear();
}

ID3D11ShaderResourceView* Renderer::modelThumbnail(const std::string& key, const live::LiveModel& model, int size, float yawDeg) {
    if (!dev || size <= 0) return white.Get();
    Thumb& th = thumbs[key];
    if (th.srv && th.size == size && th.complete && th.yaw == yawDeg) return th.srv.Get();
    if (!th.srv || th.size != size) {
        th = Thumb{};
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = static_cast<UINT>(size);
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &th.tex)) || FAILED(dev->CreateRenderTargetView(th.tex.Get(), nullptr, &th.rtv)) ||
            FAILED(dev->CreateShaderResourceView(th.tex.Get(), nullptr, &th.srv)))
            return white.Get();
        th.size = size;
    }
    if (thumbDepthSize < size) {
        thumbDepth.Reset();
        thumbDsv.Reset();
        D3D11_TEXTURE2D_DESC dd{};
        dd.Width = dd.Height = static_cast<UINT>(size);
        dd.MipLevels = dd.ArraySize = 1;
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        dd.SampleDesc.Count = 1;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(dev->CreateTexture2D(&dd, nullptr, &thumbDepth)) || FAILED(dev->CreateDepthStencilView(thumbDepth.Get(), nullptr, &thumbDsv)))
            return th.srv.Get();
        thumbDepthSize = size;
    }
    // Geometry (model space: x forward, y left, z up), batched per material.
    std::vector<Vertex> verts;
    std::vector<std::pair<std::string, std::pair<UINT, UINT>>> runs;
    bool complete = true;
    for (const live::LiveModelSurf& s : model.surfs) {
        UINT first = static_cast<UINT>(verts.size());
        for (size_t t = 0; t + 2 < s.idx.size(); t += 3)
            for (size_t k = 0; k < 3; k++) {
                size_t vi = s.idx[t + k];
                if (3 * vi + 2 >= s.pos.size()) continue;
                Vertex v{};
                std::memcpy(v.pos, &s.pos[3 * vi], 12);
                std::memcpy(v.normal, &s.nrm[3 * vi], 12);
                v.uv[0] = s.uv[2 * vi];
                v.uv[1] = s.uv[2 * vi + 1];
                v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1;
                verts.push_back(v);
            }
        runs.push_back({s.material, {first, static_cast<UINT>(verts.size()) - first}});
        texture(s.material);  // queue it
        complete &= textures.count(s.material) != 0;
    }
    // Camera: on the bounds' centre, from the front-left and a little above, distance to fit.
    const Vec3 c = (model.bounds.mins + model.bounds.maxs) * 0.5;
    const double radius = std::max(1.0, length(model.bounds.maxs - model.bounds.mins) * 0.5);
    const double fov = 40.0 * kPi / 180.0, yaw = yawDeg * kPi / 180.0, pitch = 20.0 * kPi / 180.0;
    const double dist = radius / std::sin(fov * 0.5) * 1.02;
    const Vec3 dir{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
    const Vec3 eye = c + dir * dist;
    const Vec3 f = normalize(c - eye);
    const Vec3 r = normalize(cross(f, Vec3{0, 0, 1}));
    const Vec3 u = cross(r, f);
    float view[16] = {(float)r.x, (float)r.y, (float)r.z, (float)-dot(r, eye), (float)u.x, (float)u.y, (float)u.z, (float)-dot(u, eye),
                      (float)-f.x, (float)-f.y, (float)-f.z, (float)dot(f, eye), 0, 0, 0, 1};
    const double nearZ = std::max(0.5, dist - radius * 1.5), farZ = dist + radius * 1.5;
    const float ys = (float)(1.0 / std::tan(fov * 0.5));
    float proj[16] = {ys, 0, 0, 0, 0, ys, 0, 0, 0, 0, (float)(farZ / (nearZ - farZ)), (float)(nearZ * farZ / (nearZ - farZ)), 0, 0, -1, 0};
    Constants cb{};
    multiply(proj, view, cb.viewProj);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, &cb, sizeof cb);
        ctx->Unmap(constants.Get(), 0);
    }
    const float clear[4] = {0.11f, 0.12f, 0.14f, 1};
    ctx->ClearRenderTargetView(th.rtv.Get(), clear);
    ctx->ClearDepthStencilView(thumbDsv.Get(), D3D11_CLEAR_DEPTH, 1, 0);
    if (!verts.empty()) {
        uploadVertices(thumbVb, thumbCapacity, verts);
        ID3D11RenderTargetView* rtv = th.rtv.Get();
        ctx->OMSetRenderTargets(1, &rtv, thumbDsv.Get());
        ctx->OMSetDepthStencilState(depthOn.Get(), 0);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        D3D11_VIEWPORT vp{0, 0, (float)size, (float)size, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(rasterFill.Get());
        ctx->IASetInputLayout(layout.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        UINT stride = sizeof(Vertex), offset = 0;
        ctx->IASetVertexBuffers(0, 1, thumbVb.GetAddressOf(), &stride, &offset);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
        ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
        for (const auto& [mat, range] : runs) {
            if (!range.second) continue;
            ID3D11ShaderResourceView* srv = texture(mat);
            ctx->PSSetShaderResources(0, 1, &srv);
            ctx->Draw(range.second, range.first);
        }
        ID3D11RenderTargetView* none = nullptr;
        ctx->OMSetRenderTargets(1, &none, nullptr);
    }
    th.yaw = yawDeg;
    th.complete = complete;
    return th.srv.Get();
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
    // Sky faces show the sky tint at a readable brightness (a daylight sky normalised; a dim night sky
    // stays dark).
    double m = std::max({b.env.skyColor.x, b.env.skyColor.y, b.env.skyColor.z, 0.45});
    for (int k = 0; k < 3; k++) skyColor[k] = static_cast<float>(0.85 * b.env.skyColor[k] / m);
}

void Renderer::rebuild(const Document& doc) {
    std::map<std::string, std::vector<Vertex>> byMaterial;
    std::vector<Vertex> lines;
    std::vector<Vertex> occluders;  // player clip faces (full_clip, clip_player, ...; drawn or not): block the volumetric sun

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
                if (light::blocksVolumetricSun(f.material))
                    for (size_t i = 1; i + 1 < f.winding.size(); i++)
                        for (size_t idx : {size_t(0), i, i + 1}) occluders.push_back(vertex(f.winding[idx], f.normal, 0, 0, 0, 0, 0));
                // With the lighting preview on, sky faces stay visible (drawn as sky).
                const bool sky = light::isSkyMaterial(f.material);
                const bool previewSky = sky && e == 0 && showLighting && lighting;
                if (!showToolFaces && !selected && !previewSky && gfx::isToolMaterial(f.material)) continue;
                auto& out = byMaterial[f.material];
                const light::FaceMap* lm = (e == 0 && lighting && !sky) ? lighting->find(f) : nullptr;
                // With culling on, two-sided brushes also draw each face mirrored (as compiled).
                const bool twoSided = cullBackFaces && ent.brushes[b].twoSided();
                for (int side = 0; side < (twoSided ? 2 : 1); side++) {
                    const Face& face = side ? mirroredFace(f) : f;
                    for (size_t i = 1; i + 1 < face.winding.size(); i++) {
                        for (size_t idx : {size_t(0), i, i + 1}) {
                            float u, v;
                            faceUV(face, face.winding[idx], u, v);
                            Vertex vx = vertex(face.winding[idx], face.normal, u, v, r, g, bl);
                            if (previewSky) {
                                vx.lm[0] = vx.lm[1] = -2;
                            } else if (lm && !side) {
                                lighting->lmapCoord(*lm, face.winding[idx], vx.lm[0], vx.lm[1]);
                            }
                            out.push_back(vx);
                        }
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
        Batch b{name, static_cast<UINT>(tris.size()), static_cast<UINT>(verts.size())};
        matdef::Def def;
        if (assets && assets->materialDef(name, def) && matdef::shadingFor(def) == matdef::Shading::FalloffAdd) {
            b.effect = true;
            matdef::constantValue(def, "uvAnimParms", b.fx.uvAnim);
            matdef::constantValue(def, "falloffParms", b.fx.falloff);
            matdef::constantValue(def, "falloffBegin", b.fx.beginColor);
            matdef::constantValue(def, "falloffEnd", b.fx.endColor);
        }
        batches.push_back(std::move(b));
        tris.insert(tris.end(), verts.begin(), verts.end());
    }
    occFirst = static_cast<UINT>(tris.size());
    occCount = static_cast<UINT>(occluders.size());
    tris.insert(tris.end(), occluders.begin(), occluders.end());
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
    color.Reset(); colorRtv.Reset(); colorSrv.Reset(); depth.Reset(); depthDsv.Reset(); depthSrv.Reset();
    sceneCopy.Reset(); sceneCopySrv.Reset();
    for (int i = 0; i < 2; i++) half[i].Reset(), halfRtv[i].Reset(), halfSrv[i].Reset();
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
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &sceneCopy))) return false;
    dev->CreateShaderResourceView(sceneCopy.Get(), nullptr, &sceneCopySrv);
    {  // half-res float targets for the shafts
        D3D11_TEXTURE2D_DESC hd = td;
        hd.Width = std::max(1, width / 2);
        hd.Height = std::max(1, height / 2);
        hd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (int i = 0; i < 2; i++) {
            if (FAILED(dev->CreateTexture2D(&hd, nullptr, &half[i]))) return false;
            dev->CreateRenderTargetView(half[i].Get(), nullptr, &halfRtv[i]);
            dev->CreateShaderResourceView(half[i].Get(), nullptr, &halfSrv[i]);
        }
    }
    // Depth the post passes can read: typeless storage, D24S8 view to draw, R24 view to sample.
    td.Format = DXGI_FORMAT_R24G8_TYPELESS;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &depth))) return false;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvd{};
    dsvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dev->CreateDepthStencilView(depth.Get(), &dsvd, &depthDsv);
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(depth.Get(), &srvd, &depthSrv);
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
        cb.eyePos[k] = static_cast<float>(cam.pos[k]);
    }
    cb.options[3] = static_cast<float>(static_cast<double>(GetTickCount64()) / 1000.0 - startTime);

    float clear[4] = {0.17f, 0.19f, 0.22f, 0};  // alpha 0: the void counts as sky (opaque again below)
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

    auto setOptions = [&](float unlit, const Batch* effect = nullptr) {
        cb.options[0] = unlit;
        cb.options[1] = lit ? 1.0f : 0.0f;
        cb.options[2] = effect ? 1.0f : 0.0f;
        if (effect) {
            std::memcpy(cb.uvAnim, effect->fx.uvAnim, 16);
            std::memcpy(cb.falloff, effect->fx.falloff, 16);
            std::memcpy(cb.falloffBegin, effect->fx.beginColor, 16);
            std::memcpy(cb.falloffEnd, effect->fx.endColor, 16);
        }
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
        ctx->RSSetState(cullBackFaces ? rasterCull.Get() : rasterFill.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetVertexBuffers(0, 1, triBuffer.GetAddressOf(), &stride, &offset);
        for (const Batch& b : batches) {
            if (b.effect) continue;
            ID3D11ShaderResourceView* srv = texture(b.material);
            ctx->PSSetShaderResources(0, 1, &srv);
            ctx->Draw(b.count, b.first);
        }
        // Effect materials last: additive, depth-tested but not depth-writing.
        bool anyEffect = false;
        for (const Batch& b : batches) {
            if (!b.effect) continue;
            if (!anyEffect) {
                ctx->OMSetBlendState(blendAdd.Get(), nullptr, 0xffffffff);
                ctx->OMSetDepthStencilState(depthTestOnly.Get(), 0);
                anyEffect = true;
            }
            setOptions(0, &b);
            ID3D11ShaderResourceView* srv = texture(b.material);
            ctx->PSSetShaderResources(0, 1, &srv);
            ctx->Draw(b.count, b.first);
        }
        if (anyEffect) {
            ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            ctx->OMSetDepthStencilState(depthOn.Get(), 0);
        }
    }
    if (!sprites.empty()) drawSprites(&cb);
    // Volumetric preview with the baked lighting, then make the view opaque (sky alpha was a marker).
    if (lit && showVolumetrics && vol.enabled) {
        // player clip faces: mark them "not sky" (alpha 1) where they're in front, so the sun's shafts
        // stop at them like in the game, whether they're shown or not.
        if (occCount && vol.shafts) {
            setOptions(0);
            ctx->OMSetBlendState(blendAlphaOnly.Get(), nullptr, 0xffffffff);
            ctx->OMSetDepthStencilState(depthTestOnly.Get(), 0);
            ctx->RSSetState(rasterFill.Get());
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->IASetVertexBuffers(0, 1, triBuffer.GetAddressOf(), &stride, &offset);
            ctx->PSSetShader(psSolid.Get(), nullptr, 0);
            ctx->Draw(occCount, occFirst);
            ctx->PSSetShader(ps.Get(), nullptr, 0);
            ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            ctx->OMSetDepthStencilState(depthOn.Get(), 0);
        }
        postProcess(cb.viewProj, cam);
    }
    {
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->OMSetBlendState(blendAlphaOnly.Get(), nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(depthOff.Get(), 0);
        ctx->RSSetState(rasterFill.Get());
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(postVs.Get(), nullptr, 0);
        ctx->PSSetShader(psOpaque.Get(), nullptr, 0);
        ctx->Draw(3, 0);
        // back to the scene state for the wireframes
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(depthOn.Get(), 0);
        ctx->OMSetRenderTargets(1, &rtv, depthDsv.Get());
        ctx->RSSetViewports(1, &vp);
        ctx->IASetInputLayout(layout.Get());
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
        ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
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

void Renderer::postProcess(const float viewProj[16], const FlyCamera& cam) {
    const light::Volumetric& v = vol;
    PostConstants pc{};
    if (!invert4(viewProj, pc.invViewProj)) return;
    const float sd[3] = {static_cast<float>(v.sunDir.x), static_cast<float>(v.sunDir.y), static_cast<float>(v.sunDir.z)};
    // Sun on screen (a direction: w = 0); clip.w = distance along the view axis, so it's the facing.
    float clip[4] = {0, 0, 0, 0};
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 3; c++) clip[r] += viewProj[r * 4 + c] * sd[c];
    float fade = 0, su = 0, sv = 0;
    if (clip[3] > 0.02f) {
        const float nx = clip[0] / clip[3], ny = clip[1] / clip[3];
        const float off = std::max(std::fabs(nx), std::fabs(ny));
        fade = std::min(1.0f, clip[3] / 0.35f) * std::max(0.0f, std::min(1.0f, (2.5f - off) / 1.5f));
        if (v.debug == 1) fade = 1;
        su = 0.5f + 0.5f * nx, sv = 0.5f - 0.5f * ny;
    }
    const bool shafts = v.shafts && fade > 0;
    if (!v.fog && !shafts) return;
    for (int k = 0; k < 3; k++) {
        pc.eye[k] = static_cast<float>(cam.pos[k]);
        pc.sun[k] = sd[k];
        pc.fogColor[k] = static_cast<float>(v.fogColor[k]);
        pc.fogLight[k] = static_cast<float>(v.color[k] * v.fogSun);
        pc.glow[k] = static_cast<float>(v.color[k] * v.glow);
        pc.tint[k] = static_cast<float>(v.color[k] * v.intensity);
    }
    pc.eye[3] = static_cast<float>(v.fogStart);
    pc.sun[3] = static_cast<float>(std::clamp(v.fogG, -0.95, 0.95));
    pc.fogColor[3] = static_cast<float>(v.fogMax);
    pc.fogLight[3] = static_cast<float>(v.fogSky);
    pc.fogParm[0] = static_cast<float>(v.fogDensity);
    pc.fogParm[1] = static_cast<float>(v.fogFalloff);
    pc.fogParm[2] = static_cast<float>(v.fogHeight);
    pc.fogParm[3] = static_cast<float>(v.fogSkyDist);
    pc.sunUv[0] = su;
    pc.sunUv[1] = sv;
    pc.sunUv[2] = static_cast<float>(aspect());
    pc.sunUv[3] = static_cast<float>(std::max(0.005, v.glowRadius));
    pc.mask[0] = static_cast<float>(v.threshold);
    pc.mask[1] = 1;
    pc.tint[3] = fade;
    pc.flags[0] = static_cast<float>(v.debug);
    auto upload = [&] {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(postConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, &pc, sizeof pc);
            ctx->Unmap(postConstants.Get(), 0);
        }
    };
    upload();

    ID3D11ShaderResourceView* noSrv[3] = {nullptr, nullptr, nullptr};
    ctx->PSSetShaderResources(0, 3, noSrv);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);  // depth is sampled below
    ctx->CopyResource(sceneCopy.Get(), color.Get());
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(postVs.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, postConstants.GetAddressOf());
    ctx->PSSetSamplers(0, 1, lmSampler.GetAddressOf());  // linear clamp
    ctx->OMSetDepthStencilState(depthOff.Get(), 0);
    ctx->RSSetState(rasterFill.Get());
    const D3D11_VIEWPORT full{0, 0, (float)viewWidth, (float)viewHeight, 0, 1};
    const D3D11_VIEWPORT halfVp{0, 0, (float)std::max(1, viewWidth / 2), (float)std::max(1, viewHeight / 2), 0, 1};
    auto pass = [&](ID3D11RenderTargetView* target, const D3D11_VIEWPORT& port, ID3D11PixelShader* shader, ID3D11ShaderResourceView* in0,
                    ID3D11ShaderResourceView* in1, ID3D11BlendState* blend) {
        ctx->OMSetRenderTargets(1, &target, nullptr);
        ctx->RSSetViewports(1, &port);
        ctx->OMSetBlendState(blend, nullptr, 0xffffffff);
        ID3D11ShaderResourceView* in[2] = {in0, in1};
        ctx->PSSetShaderResources(0, 2, in);
        ctx->PSSetShader(shader, nullptr, 0);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
        ctx->PSSetShaderResources(0, 2, none);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
    };
    if (v.fog) pass(colorRtv.Get(), full, psFog.Get(), sceneCopySrv.Get(), depthSrv.Get(), nullptr);
    if (shafts) {
        if (v.debug == 1) {
            pass(colorRtv.Get(), full, psMask.Get(), sceneCopySrv.Get(), nullptr, nullptr);
        } else {
            pass(halfRtv[0].Get(), halfVp, psMask.Get(), sceneCopySrv.Get(), nullptr, nullptr);
            pc.blur[0] = static_cast<float>(v.density);
            pc.blur[1] = static_cast<float>(v.decay);
            pc.blur[2] = 1;
            upload();
            pass(halfRtv[1].Get(), halfVp, psBlur.Get(), halfSrv[0].Get(), nullptr, nullptr);
            pc.blur[0] = static_cast<float>(v.density / 32);
            pc.blur[1] = 1;
            upload();
            pass(halfRtv[0].Get(), halfVp, psBlur.Get(), halfSrv[1].Get(), nullptr, nullptr);
            pass(colorRtv.Get(), full, psComposite.Get(), halfSrv[0].Get(), nullptr, blendScreen.Get());
        }
    }
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->RSSetViewports(1, &full);
}
