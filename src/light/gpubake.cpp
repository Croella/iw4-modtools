#include "light/gpubake.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>

namespace light::gpubake {
namespace {

using Microsoft::WRL::ComPtr;

// Same traversal and intersection as the CPU Scene (lightbake.cpp): BVH with a 4-tri leaf limit,
// slab test, two-sided Moller-Trumbore, hits nearer than 1e-3 ignored.
const char kShader[] = R"(
struct Tri { float3 a; float3 e1; float3 e2; int face; int sky; };
struct Node { float3 mn; int start; float3 mx; int count; };
struct Map { int x; int y; int w; int h; int axisU; int axisV; float minU; float minV; };
struct Sample { float3 p; float3 n; int skip; };
struct VisSample { float3 p; float3 t; int isPoint; };
struct Light { float3 pos; float radius; float3 color; float spot; float3 dir; float cosOuter; float cosInner; float3 pad; };
StructuredBuffer<Tri> tris : register(t0);
StructuredBuffer<Node> nodes : register(t1);
StructuredBuffer<Map> maps : register(t2);
StructuredBuffer<Sample> samples : register(t3);
StructuredBuffer<float4> source : register(t4);
StructuredBuffer<Light> lightList : register(t5);
StructuredBuffer<VisSample> visSamples : register(t6);
RWStructuredBuffer<float4> outRgb : register(u0);
RWStructuredBuffer<uint> outVis : register(u1);
RWStructuredBuffer<float4> outDir : register(u2);
RWStructuredBuffer<float4> outSkip : register(u3);
float lum(float3 c) { return dot(c, float3(0.3, 0.59, 0.11)); }
cbuffer C : register(b0) {
    float3 sunDir; uint first;
    float3 sky; uint chunkEnd;
    uint width; float ls; uint rays; float albedo;
    uint seed; uint nLights; uint bounce; uint pad0;
};

bool trace(float3 o, float3 d, float tmax, bool any, out float tHit, out int triHit) {
    float3 inv = float3(abs(d.x) > 1e-12 ? 1.0 / d.x : 1e30, abs(d.y) > 1e-12 ? 1.0 / d.y : 1e30, abs(d.z) > 1e-12 ? 1.0 / d.z : 1e30);
    tHit = tmax;
    triHit = -1;
    int stack[64];
    int sp = 0;
    stack[sp++] = 0;
    [loop] while (sp > 0) {
        Node n = nodes[stack[--sp]];
        float3 a = (n.mn - o) * inv, b = (n.mx - o) * inv;
        float3 lo = min(a, b), hi = max(a, b);
        float t0 = max(max(lo.x, lo.y), max(lo.z, 0)), t1 = min(min(hi.x, hi.y), min(hi.z, tHit));
        if (t0 > t1) continue;
        if (n.count == 0) {
            if (sp + 2 <= 64) {
                stack[sp++] = n.start;
                stack[sp++] = n.start + 1;
            }
            continue;
        }
        [loop] for (int k = n.start; k < n.start + n.count; k++) {
            Tri t = tris[k];
            float3 pv = cross(d, t.e2);
            float det = dot(t.e1, pv);
            if (abs(det) < 1e-9) continue;
            float id = 1.0 / det;
            float3 s = o - t.a;
            float u = dot(s, pv) * id;
            if (u < 0 || u > 1) continue;
            float3 q = cross(s, t.e1);
            float v = dot(d, q) * id;
            if (v < 0 || u + v > 1) continue;
            float tt = dot(t.e2, q) * id;
            if (tt <= 1e-3 || tt >= tHit) continue;
            tHit = tt;
            triHit = k;
            if (any) return true;
        }
    }
    return triHit >= 0;
}

[numthreads(64, 1, 1)]
void csSun(uint3 id : SV_DispatchThreadID) {
    uint i = first + id.x;
    if (i >= chunkEnd) return;
    VisSample s = visSamples[i];
    float t;
    int k;
    if (s.isPoint != 0) {
        float3 d = s.t - s.p;
        float dist = length(d);
        outVis[i] = (dist < 1e-6 || !trace(s.p, d / dist, dist - 1.0, true, t, k)) ? 255 : 0;
        return;
    }
    bool hit = trace(s.p, s.t, 1e6, false, t, k);
    outVis[i] = (!hit || tris[k].sky != 0) ? 255 : 0;
}

[numthreads(64, 1, 1)]
void csLights(uint3 id : SV_DispatchThreadID) {
    uint i = first + id.x;
    if (i >= chunkEnd) return;
    Sample s = samples[i];
    float3 sum = 0, dsum = 0, skipSum = 0;
    float wsum = 0;
    [loop] for (uint j = 0; j < nLights; j++) {
        Light L = lightList[j];
        float3 dv = L.pos - s.p;
        float dist = length(dv);
        if (dist >= L.radius || dist < 1e-6) continue;
        float3 l = dv / dist;
        float ndl = dot(s.n, l);
        if (ndl <= 0) continue;
        float f = 1 - (dist * dist) / (L.radius * L.radius);
        float atten = f * f;
        if (L.spot > 0.5) {
            float c = -dot(l, L.dir);
            float t = saturate((c - L.cosOuter) / max(L.cosInner - L.cosOuter, 1e-6));
            atten *= t * t * (3 - 2 * t);
            if (atten <= 0) continue;
        }
        float th;
        int tk;
        if (trace(s.p, l, dist - 1.0, true, th, tk)) continue;
        float3 c = L.color * (atten * ndl);
        if ((int)j == s.skip) {
            skipSum += c;
            continue;
        }
        sum += c;
        dsum += l * lum(c);
        wsum += lum(c);
    }
    outRgb[i] = float4(sum, 0);
    outDir[i] = float4(dsum, wsum);
    outSkip[i] = float4(skipSum, 0);
}

uint pcg(uint v) {
    uint st = v * 747796405u + 2891336453u;
    uint w = ((st >> ((st >> 28u) + 4u)) ^ st) * 277803737u;
    return (w >> 22u) ^ w;
}
float rnd(inout uint st) {
    st = pcg(st);
    return (st >> 8) * (1.0 / 16777216.0);
}
float axisOf(float3 p, int a) { return a == 0 ? p.x : (a == 1 ? p.y : p.z); }

[numthreads(64, 1, 1)]
void csGather(uint3 id : SV_DispatchThreadID) {
    uint i = first + id.x;
    if (i >= chunkEnd) return;
    Sample s = samples[i];
    float3 n = s.n;
    float3 t = normalize(abs(n.z) < 0.9 ? cross(n, float3(0, 0, 1)) : cross(n, float3(1, 0, 0)));
    float3 b = cross(n, t);
    uint st = pcg(i ^ pcg(seed));
    int side = max(1, (int)sqrt((float)rays));
    float3 skySum = 0, bounceSum = 0, dsum = 0;
    float wsum = 0, skyW = lum(sky);
    [loop] for (uint r = 0; r < rays; r++) {
        float u1 = ((r % side) + rnd(st)) / side, u2 = (((r / side) % side) + rnd(st)) / side;
        float rr = sqrt(u1), phi = 6.28318530718 * u2;
        float3 dir = normalize(t * (rr * cos(phi)) + b * (rr * sin(phi)) + n * sqrt(max(0, 1 - u1)));
        float th;
        int tk;
        if (!trace(s.p, dir, 1e6, false, th, tk) || tris[tk].sky != 0) {
            skySum += sky;
            dsum += dir * skyW;
            wsum += skyW;
        } else if (bounce != 0) {
            int f = tris[tk].face;
            if (f >= 0) {
                float3 hp = s.p + dir * th;
                Map m = maps[f];
                int hx = clamp((int)((axisOf(hp, m.axisU) - m.minU) / ls), 0, m.w - 1) + m.x;
                int hy = clamp((int)((axisOf(hp, m.axisV) - m.minV) / ls), 0, m.h - 1) + m.y;
                float3 bc = source[hy * width + hx].rgb * albedo;
                bounceSum += bc;
                dsum += dir * lum(bc);
                wsum += lum(bc);
            }
        }
    }
    outRgb[i] = float4((skySum + bounceSum) / max(rays, 1u), 0);
    outDir[i] = float4(dsum, wsum) / max(rays, 1u);
}
)";

struct Consts {
    float sunDir[3];
    uint32_t first;
    float sky[3];
    uint32_t chunkEnd;
    uint32_t width;
    float ls;
    uint32_t rays;
    float albedo;
    uint32_t seed, nLights, bounce, pad0;
};
static_assert(sizeof(Consts) == 64, "cbuffer layout");

// Compiled once per process.
struct Blobs {
    std::mutex m;
    bool tried = false;
    ComPtr<ID3DBlob> sun, lights, gather;
    std::string error;
};
Blobs& blobs() {
    static Blobs b;
    return b;
}

std::string narrow(const wchar_t* w) {
    std::string s;
    for (; *w; w++) s += *w < 128 ? static_cast<char>(*w) : '?';
    return s;
}

}  // namespace

struct Context::Impl {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> csSun, csLights, csGather;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11ShaderResourceView> tris, nodes, maps;
    ComPtr<ID3D11Query> done;
    int width = 0;
    float ls = 16;
    double rayRate = 0;  // rays per second measured so far (sizes the chunks)

    bool structured(const void* data, UINT stride, UINT count, ComPtr<ID3D11ShaderResourceView>& srv, std::string& error) {
        count = std::max(count, 1u);
        std::vector<uint8_t> zero;
        if (!data) {
            zero.assign(static_cast<size_t>(stride) * count, 0);
            data = zero.data();
        }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = stride * count;
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = stride;
        D3D11_SUBRESOURCE_DATA sd{data, 0, 0};
        ComPtr<ID3D11Buffer> buf;
        if (FAILED(dev->CreateBuffer(&bd, &sd, &buf)) || FAILED(dev->CreateShaderResourceView(buf.Get(), nullptr, &srv))) {
            error = "couldn't create a GPU buffer (" + std::to_string(bd.ByteWidth) + " bytes)";
            return false;
        }
        return true;
    }

    // Default-usage structured buffer + UAV, and a staging twin to read it back.
    bool output(UINT stride, UINT count, ComPtr<ID3D11Buffer>& buf, ComPtr<ID3D11UnorderedAccessView>& uav, ComPtr<ID3D11Buffer>& staging,
                std::string& error) {
        count = std::max(count, 1u);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = stride * count;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = stride;
        D3D11_BUFFER_DESC sd = bd;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.MiscFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &buf)) || FAILED(dev->CreateUnorderedAccessView(buf.Get(), nullptr, &uav)) ||
            FAILED(dev->CreateBuffer(&sd, nullptr, &staging))) {
            error = "couldn't create a GPU output buffer";
            return false;
        }
        return true;
    }

    void wait() {
        ctx->End(done.Get());
        BOOL ok = FALSE;
        int spins = 0;
        while (ctx->GetData(done.Get(), &ok, sizeof ok, 0) == S_FALSE) {
            if (++spins < 200) SwitchToThread();
            else Sleep(1);
        }
    }

    // Runs `cs` over [0, count) in chunks of about 120 ms each (sized from the measured ray rate),
    // `raysEach` rays per item. Constants are filled by the caller except first/chunkEnd.
    bool run(ID3D11ComputeShader* cs, Consts c, UINT count, double raysEach, Tick tick, void* user, std::string& error) {
        ctx->CSSetShader(cs, nullptr, 0);
        UINT at = 0;
        while (at < count) {
            const double rate = rayRate > 0 ? rayRate : 2e7;  // first guess: 20 M rays/s
            UINT chunk = static_cast<UINT>(std::clamp(rate * 0.12 / std::max(1.0, raysEach), 64.0, 64.0 * 65535));
            chunk = std::min(chunk, count - at);
            c.first = at;
            c.chunkEnd = at + chunk;
            ctx->UpdateSubresource(cb.Get(), 0, nullptr, &c, 0, 0);
            const auto t0 = std::chrono::steady_clock::now();
            ctx->Dispatch((chunk + 63) / 64, 1, 1);
            wait();
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (secs > 1e-4) {
                const double r = chunk * std::max(1.0, raysEach) / secs;
                rayRate = rayRate > 0 ? rayRate * 0.5 + r * 0.5 : r;
            }
            if (dev->GetDeviceRemovedReason() != S_OK) {
                error = "the GPU device was lost (driver timeout?)";
                return false;
            }
            at += chunk;
            if (tick && !tick(user, static_cast<float>(at) / count)) {
                error = "cancelled";
                return false;
            }
        }
        return true;
    }

    bool read(ID3D11Buffer* buf, ID3D11Buffer* staging, void* out, size_t bytes, std::string& error) {
        ctx->CopyResource(staging, buf);
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            error = "couldn't read the GPU results back";
            return false;
        }
        std::memcpy(out, m.pData, bytes);
        ctx->Unmap(staging, 0);
        return true;
    }

    void bindScene(ID3D11ShaderResourceView* samplesSrv, ID3D11ShaderResourceView* sourceSrv, ID3D11ShaderResourceView* lightSrv,
                   ID3D11UnorderedAccessView* rgb, ID3D11UnorderedAccessView* vis, ID3D11UnorderedAccessView* dir,
                   ID3D11ShaderResourceView* visSrv = nullptr, ID3D11UnorderedAccessView* skip = nullptr) {
        ID3D11ShaderResourceView* srv[7] = {tris.Get(), nodes.Get(), maps.Get(), samplesSrv, sourceSrv, lightSrv, visSrv};
        ctx->CSSetShaderResources(0, 7, srv);
        ID3D11UnorderedAccessView* uav[4] = {rgb, vis, dir, skip};
        ctx->CSSetUnorderedAccessViews(0, 4, uav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
    }
    void unbind() {
        ID3D11ShaderResourceView* srv[7] = {};
        ctx->CSSetShaderResources(0, 7, srv);
        ID3D11UnorderedAccessView* uav[4] = {};
        ctx->CSSetUnorderedAccessViews(0, 4, uav, nullptr);
    }
};

Context::Context() : d(std::make_unique<Impl>()) {}
Context::~Context() = default;

bool Context::init(std::string& device, std::string& error) {
    {
        Blobs& b = blobs();
        std::lock_guard<std::mutex> lock(b.m);
        if (!b.tried) {
            b.tried = true;
            for (auto [entry, out] : {std::pair{"csSun", std::addressof(b.sun)}, std::pair{"csLights", std::addressof(b.lights)},
                                      std::pair{"csGather", std::addressof(b.gather)}}) {
                ComPtr<ID3DBlob> errs;
                if (FAILED(D3DCompile(kShader, sizeof kShader - 1, "gpubake", nullptr, nullptr, entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                      out->GetAddressOf(), &errs))) {
                    b.error = std::string("kernel ") + entry + " didn't compile" +
                              (errs ? ": " + std::string(static_cast<const char*>(errs->GetBufferPointer()), errs->GetBufferSize()) : "");
                    out->Reset();
                    break;
                }
            }
        }
        if (!b.error.empty()) {
            error = b.error;
            return false;
        }
    }
    const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL got{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &want, 1, D3D11_SDK_VERSION, &d->dev, &got, &d->ctx))) {
        error = "no Direct3D 11 GPU";
        return false;
    }
    device = "GPU";
    ComPtr<IDXGIDevice> dx;
    ComPtr<IDXGIAdapter> ad;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(d->dev.As(&dx)) && SUCCEEDED(dx->GetAdapter(&ad)) && SUCCEEDED(ad->GetDesc(&desc))) device = narrow(desc.Description);
    Blobs& b = blobs();
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(Consts);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    if (FAILED(d->dev->CreateComputeShader(b.sun->GetBufferPointer(), b.sun->GetBufferSize(), nullptr, &d->csSun)) ||
        FAILED(d->dev->CreateComputeShader(b.lights->GetBufferPointer(), b.lights->GetBufferSize(), nullptr, &d->csLights)) ||
        FAILED(d->dev->CreateComputeShader(b.gather->GetBufferPointer(), b.gather->GetBufferSize(), nullptr, &d->csGather)) ||
        FAILED(d->dev->CreateBuffer(&cbd, nullptr, &d->cb)) || FAILED(d->dev->CreateQuery(&qd, &d->done))) {
        error = "couldn't create the GPU kernels";
        return false;
    }
    return true;
}

bool Context::setScene(const std::vector<Tri>& tris, const std::vector<Node>& nodes, const std::vector<Map>& maps, int width, float ls,
                       std::string& error) {
    d->width = width;
    d->ls = ls;
    return d->structured(tris.empty() ? nullptr : tris.data(), sizeof(Tri), static_cast<UINT>(tris.size()), d->tris, error) &&
           d->structured(nodes.empty() ? nullptr : nodes.data(), sizeof(Node), static_cast<UINT>(nodes.size()), d->nodes, error) &&
           d->structured(maps.empty() ? nullptr : maps.data(), sizeof(Map), static_cast<UINT>(maps.size()), d->maps, error);
}

bool Context::sun(const std::vector<VisSample>& samples, std::vector<uint8_t>& vis, Tick tick, void* user, std::string& error) {
    vis.assign(samples.size(), 0);
    if (samples.empty()) return true;
    const UINT n = static_cast<UINT>(samples.size());
    ComPtr<ID3D11ShaderResourceView> sSrv, none, noLights;
    ComPtr<ID3D11Buffer> out, staging, out2, staging2, out3, staging3;
    ComPtr<ID3D11UnorderedAccessView> uVis, uRgb, uDir;
    ComPtr<ID3D11ShaderResourceView> noSamples;
    ComPtr<ID3D11Buffer> out4, staging4;
    ComPtr<ID3D11UnorderedAccessView> uSkip;
    if (!d->structured(samples.data(), sizeof(VisSample), n, sSrv, error) || !d->structured(nullptr, 16, 1, none, error) ||
        !d->structured(nullptr, sizeof(Light), 1, noLights, error) || !d->structured(nullptr, sizeof(Sample), 1, noSamples, error) ||
        !d->output(4, n, out, uVis, staging, error) || !d->output(16, 1, out2, uRgb, staging2, error) ||
        !d->output(16, 1, out3, uDir, staging3, error) || !d->output(16, 1, out4, uSkip, staging4, error))
        return false;
    d->bindScene(noSamples.Get(), none.Get(), noLights.Get(), uRgb.Get(), uVis.Get(), uDir.Get(), sSrv.Get(), uSkip.Get());
    Consts c{};
    c.width = static_cast<uint32_t>(d->width);
    c.ls = d->ls;
    const bool ok = d->run(d->csSun.Get(), c, n, 1, tick, user, error);
    d->unbind();
    if (!ok) return false;
    std::vector<uint32_t> raw(n);
    if (!d->read(out.Get(), staging.Get(), raw.data(), raw.size() * 4, error)) return false;
    for (UINT i = 0; i < n; i++) vis[i] = static_cast<uint8_t>(raw[i]);
    return true;
}

bool Context::lights(const std::vector<Sample>& samples, const std::vector<Light>& lights, std::vector<float>& rgb,
                     std::vector<float>& skipRgb, std::vector<float>& dir, Tick tick, void* user, std::string& error) {
    rgb.assign(samples.size() * 3, 0.0f);
    skipRgb.assign(samples.size() * 3, 0.0f);
    dir.assign(samples.size() * 4, 0.0f);
    if (samples.empty() || lights.empty()) return true;
    const UINT n = static_cast<UINT>(samples.size());
    ComPtr<ID3D11ShaderResourceView> sSrv, none, lSrv, noVis;
    ComPtr<ID3D11Buffer> out, staging, out2, staging2, out3, staging3, out4, staging4;
    ComPtr<ID3D11UnorderedAccessView> uRgb, uVis, uDir, uSkip;
    if (!d->structured(samples.data(), sizeof(Sample), n, sSrv, error) || !d->structured(nullptr, 16, 1, none, error) ||
        !d->structured(lights.data(), sizeof(Light), static_cast<UINT>(lights.size()), lSrv, error) ||
        !d->output(16, n, out, uRgb, staging, error) || !d->output(4, 1, out2, uVis, staging2, error) ||
        !d->output(16, n, out3, uDir, staging3, error) || !d->output(16, n, out4, uSkip, staging4, error) ||
        !d->structured(nullptr, sizeof(VisSample), 1, noVis, error))
        return false;
    d->bindScene(sSrv.Get(), none.Get(), lSrv.Get(), uRgb.Get(), uVis.Get(), uDir.Get(), noVis.Get(), uSkip.Get());
    Consts c{};
    c.width = static_cast<uint32_t>(d->width);
    c.ls = d->ls;
    c.nLights = static_cast<uint32_t>(lights.size());
    // Most lights are out of range of most luxels: count a quarter of a ray per light.
    const bool ok = d->run(d->csLights.Get(), c, n, std::max(1.0, lights.size() * 0.25), tick, user, error);
    d->unbind();
    if (!ok) return false;
    std::vector<float> raw(static_cast<size_t>(n) * 4);
    if (!d->read(out.Get(), staging.Get(), raw.data(), raw.size() * 4, error)) return false;
    for (UINT i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) rgb[i * 3 + k] = raw[i * 4 + k];
    if (!d->read(out4.Get(), staging4.Get(), raw.data(), raw.size() * 4, error)) return false;
    for (UINT i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) skipRgb[i * 3 + k] = raw[i * 4 + k];
    return d->read(out3.Get(), staging3.Get(), dir.data(), dir.size() * 4, error);
}

bool Context::gather(const std::vector<Sample>& samples, const std::vector<float>& source, const float skyColor[3], int rays, float albedo,
                     bool bounce, uint32_t seed, std::vector<float>& rgb, std::vector<float>& dir, Tick tick, void* user,
                     std::string& error) {
    rgb.assign(samples.size() * 3, 0.0f);
    dir.assign(samples.size() * 4, 0.0f);
    if (samples.empty()) return true;
    const UINT n = static_cast<UINT>(samples.size());
    std::vector<float> src4(source.size() / 3 * 4, 0.0f);
    for (size_t i = 0; i < source.size() / 3; i++)
        for (int k = 0; k < 3; k++) src4[i * 4 + k] = source[i * 3 + k];
    ComPtr<ID3D11ShaderResourceView> sSrv, srcSrv, noLights;
    ComPtr<ID3D11Buffer> out, staging, out2, staging2, out3, staging3;
    ComPtr<ID3D11UnorderedAccessView> uRgb, uVis, uDir;
    if (!d->structured(samples.data(), sizeof(Sample), n, sSrv, error) ||
        !d->structured(src4.empty() ? nullptr : src4.data(), 16, static_cast<UINT>(src4.size() / 4), srcSrv, error) ||
        !d->structured(nullptr, sizeof(Light), 1, noLights, error) || !d->output(16, n, out, uRgb, staging, error) ||
        !d->output(4, 1, out2, uVis, staging2, error) || !d->output(16, n, out3, uDir, staging3, error))
        return false;
    d->bindScene(sSrv.Get(), srcSrv.Get(), noLights.Get(), uRgb.Get(), uVis.Get(), uDir.Get());
    Consts c{};
    std::memcpy(c.sky, skyColor, sizeof c.sky);
    c.width = static_cast<uint32_t>(d->width);
    c.ls = d->ls;
    c.rays = static_cast<uint32_t>(std::max(1, rays));
    c.albedo = albedo;
    c.bounce = bounce ? 1 : 0;
    c.seed = seed;
    const bool ok = d->run(d->csGather.Get(), c, n, c.rays, tick, user, error);
    d->unbind();
    if (!ok) return false;
    std::vector<float> raw(static_cast<size_t>(n) * 4);
    if (!d->read(out.Get(), staging.Get(), raw.data(), raw.size() * 4, error)) return false;
    for (UINT i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) rgb[i * 3 + k] = raw[i * 4 + k];
    return d->read(out3.Get(), staging3.Get(), dir.data(), dir.size() * 4, error);
}

}  // namespace light::gpubake
