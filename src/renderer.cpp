#include "renderer.h"
#include "zone/gfxworld.h"
#include "patch.h"
#include "zone/matdefs.h"
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
    float4 aoParm;    // x radius, y intensity, z bias, w 1 = AO only
    float4 aoFade;    // x near, y far, z 1/width, w 1/height
    float4 aoBlur;    // xy one texel along the blur direction; zw projection x/y scale
    float4 grade;     // x table size, y vignette, z aspect
    float4 dofDist;   // nearStart, nearEnd, farStart, farEnd (offsets from the focus in auto mode)
    float4 dofBlurP;  // x near max CoC, y far max CoC (half-res px), z weapon distance, w 1 = show the blur amount
    float4 dofStep;   // xy half-res texel, zw full-res texel
    float4 dofAutoP;  // x 1 = auto focus, y ease factor this frame, z distance the sky counts as
    float4 flareSun;  // xy sun uv, z aspect, w strength
    float4 flareCol;  // rgb colour, w ghosts
    float4 flareParm; // x halo, y streak, z veiling glare, w visibility disc radius
    float4 flareMisc; // x visibility ease factor
    float4 glareParm; // x threshold, y intensity, zw blur step
    float4 wxParm;    // x 1 rain / 2 snow, y density, z time, w farthest layer
    float4 wxWind;    // xy wind, z fall speed, w pixel size at distance 1
    float4 wxColor;   // rgb colour, w opacity
    float4 lPos;      // local light: xyz position, w radius
    float4 lCol;      // rgb colour * intensity * density
    float4 lSpot;     // xyz spot direction, w cos outer (< -1.5: point light)
    float4 lMisc;     // x cos inner, y shadow strength, z jitter, w 1 = shadow pass on
    float4 lScreen;   // xy light uv, z shadow reach
    float4 csParm;    // contact shadows: x intensity, y thickness, z jitter, w length
    float4 csFade;    // x near, y far, z 1/width, w 1/height
    row_major float4x4 viewProjM;
};
Texture2D src : register(t0);
Texture2D<float> depthTex : register(t1);
Texture3D lutTex : register(t2);
Texture2D<float> focusTex : register(t3);
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
// Ambient occlusion: same model as iw4r_mapload.dll's (tools/shaders/post/volumetric.hlsl psAo), on
// world positions from the depth buffer; sky (alpha 0) and the void get none.
float3 worldAt(float2 uv, out bool sky) {
    int2 px = clamp(int2(uv / aoFade.zw), int2(0, 0), int2(1 / aoFade.zw) - 1);
    float d = depthTex.Load(int3(px, 0));
    sky = d >= 1;
    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    float4 w = mul(invViewProj, float4(ndc, d, 1));
    return w.xyz / w.w;
}
float3 worldAt(float2 uv) { bool s; return worldAt(uv, s); }
#define AO_TAPS 16
float4 psAo(V i) : SV_Target {
    bool sky;
    float3 P = worldAt(i.uv, sky);
    if (sky || src.SampleLevel(lin, i.uv, 0).a < 0.5) return 1;
    float z = length(P - eye.xyz);
    if (z < aoFade.x * 0.5) return 1;
    float2 tx = float2(aoFade.z, 0), ty = float2(0, aoFade.w);
    float3 pr = worldAt(i.uv + tx), pl = worldAt(i.uv - tx), pd = worldAt(i.uv + ty), pu = worldAt(i.uv - ty);
    float3 dx = dot(pr - P, pr - P) < dot(P - pl, P - pl) ? pr - P : P - pl;
    float3 dy = dot(pd - P, pd - P) < dot(P - pu, P - pu) ? pd - P : P - pu;
    float3 n = normalize(cross(dx, dy));
    if (dot(n, P - eye.xyz) > 0) n = -n;
    float r = aoParm.x, r2 = r * r;
    float2 scale = 0.5 * aoBlur.zw * r / z;
    scale *= min(1, 0.25 / max(scale.y, 1e-5));
    float ang = 6.2831853 * frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715))));
    float sum = 0;
    [unroll] for (int t = 0; t < AO_TAPS; t++) {
        float f = (t + 0.5) / AO_TAPS;
        float a = ang + t * 2.3999632;
        float3 v = worldAt(i.uv + float2(cos(a), sin(a)) * f * scale) - P;
        float vv = dot(v, v);
        sum += max(dot(v, n) * rsqrt(vv + 1e-4) - aoParm.z, 0) * saturate(1 - vv / r2);
    }
    float ao = saturate(1 - aoParm.y * 2 * sum / (AO_TAPS * (1 - aoParm.z)));
    float fade = saturate((z - aoFade.x) / max(aoFade.x * 0.5, 1)) * saturate((aoFade.y - z) / (aoFade.y * 0.25));
    return lerp(1, ao, fade);
}
float4 psAoBlur(V i) : SV_Target {
    float zc = length(worldAt(i.uv) - eye.xyz);
    float sum = 0, wsum = 0;
    [unroll] for (int t = -4; t <= 4; t++) {
        float2 u = i.uv + aoBlur.xy * t;
        float zi = length(worldAt(u) - eye.xyz);
        float w = (5 - abs(t)) * saturate(1 - abs(zi - zc) / (0.04 * zc + 2));
        sum += src.SampleLevel(lin, u, 0).r * w;
        wsum += w;
    }
    float ao = wsum > 0 ? sum / wsum : 1;
    return float4(ao, ao, ao, 1);
}
// Contact shadows: same model as iw4r_mapload.dll's (volumetric.hlsl psContact), in world space.
#define CS_STEPS 16
float4 psContact(V i) : SV_Target {
    bool sky;
    float3 P = worldAt(i.uv, sky);
    if (sky || src.SampleLevel(lin, i.uv, 0).a < 0.5) return 1;
    float z = length(P - eye.xyz);
    if (z < csFade.x) return 1;
    float2 tx = float2(csFade.z, 0), ty = float2(0, csFade.w);
    float3 pr = worldAt(i.uv + tx), pl = worldAt(i.uv - tx), pd = worldAt(i.uv + ty), pu = worldAt(i.uv - ty);
    float3 dx = dot(pr - P, pr - P) < dot(P - pl, P - pl) ? pr - P : P - pl;
    float3 dy = dot(pd - P, pd - P) < dot(P - pu, P - pu) ? pd - P : P - pu;
    float3 n = normalize(cross(dx, dy));
    if (dot(n, P - eye.xyz) > 0) n = -n;
    float ndl = dot(n, sun.xyz);
    if (ndl <= 0.02) return 1;
    float j = frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715))) + csParm.z);
    float3 O = P + n * (0.5 + 0.002 * z);
    float occ = 0;
    [loop] for (int t = 0; t < CS_STEPS; t++) {
        float f = (t + j) / CS_STEPS;
        float3 Q = O + sun.xyz * (csParm.w * f * f + 0.5);
        float4 c = mul(viewProjM, float4(Q, 1));
        if (c.w < csFade.x) break;
        float2 q = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
        if (q.x < 0 || q.x > 1 || q.y < 0 || q.y > 1) break;
        if (dot((q - i.uv) / csFade.zw, (q - i.uv) / csFade.zw) < 4) continue;  // the pixel's own neighbours (depth edges)
        bool qs;
        float3 S = worldAt(q, qs);
        if (qs) continue;
        float ds = length(S - eye.xyz);
        if (ds < csFade.x) continue;
        float dq = length(Q - eye.xyz) - ds;
        if (dq > 0.003 * ds + 0.3 && dq < csParm.y) {
            occ = 1 - f * f * 0.6;
            break;
        }
    }
    float fade = saturate((csFade.y - z) / (csFade.y * 0.25)) * saturate(ndl * 4);
    return 1 - csParm.x * occ * fade;
}
// Depth of field: same model as iw4r_mapload.dll's (volumetric.hlsl psDof*), on the distance from
// the camera.
float viewDist(float2 uv) {
    bool sky;
    float3 P = worldAt(uv, sky);
    return sky ? 1e7 : length(P - eye.xyz);
}
float4 dofPlanes() {
    if (dofAutoP.x < 0.5) return dofDist;
    float f = focusTex.Load(int3(0, 0, 0));
    return float4(max(f - dofDist.x, 0), max(f - dofDist.y, 0), f + dofDist.z, f + dofDist.w);
}
float dofCoc(float z, float4 pl) {
    if (z < dofBlurP.z) return 0;
    float n = dofBlurP.x * saturate((pl.y - z) / max(pl.y - pl.x, 1));
    float f = dofBlurP.y * saturate((z - pl.z) / max(pl.w - pl.z, 1));
    return max(n, f);
}
float4 psDofFocus(V i) : SV_Target {
    float sum = 0;
    [unroll] for (int t = 0; t < 5; t++) {
        float2 o = t == 0 ? float2(0, 0) : float2(t == 1 ? 0.02 : t == 2 ? -0.02 : 0, t == 3 ? 0.03 : t == 4 ? -0.03 : 0);
        float z = viewDist(float2(0.5, 0.5) + o);
        sum += z < dofBlurP.z ? dofAutoP.z : min(z, dofAutoP.z);
    }
    float cur = sum / 5;
    float prev = src.Load(int3(0, 0, 0)).r;
    float f = prev > 0 ? lerp(prev, cur, dofAutoP.y) : cur;
    return float4(f, f, f, 1);
}
float4 psDofDown(V i) : SV_Target {
    float3 c = src.SampleLevel(lin, i.uv, 0).rgb;
    return float4(c, dofCoc(viewDist(i.uv), dofPlanes()));
}
#define DOF_TAPS 24
float4 psDofBlur(V i) : SV_Target {
    float4 c0 = src.SampleLevel(lin, i.uv, 0);
    float cc = c0.a, zc = viewDist(i.uv);
    float R = max(dofBlurP.x, dofBlurP.y);
    float3 sum = c0.rgb;
    float wsum = 1, csum = cc;
    [unroll] for (int t = 0; t < DOF_TAPS; t++) {
        float r = sqrt((t + 0.5) / DOF_TAPS);
        float a = t * 2.3999632;
        float dist = r * R;
        float2 u = i.uv + float2(cos(a), sin(a)) * dist * dofStep.xy;
        float4 s = src.SampleLevel(lin, u, 0);
        float eff = viewDist(u) > zc ? min(s.a, cc) : s.a;
        float w = saturate(eff - dist + 1);
        sum += s.rgb * w;
        wsum += w;
        csum += eff * w;
    }
    return float4(sum / wsum, csum / wsum);
}
Texture2D blurTex : register(t4);
float4 psDofComp(V i) : SV_Target {
    float4 sharp = src.Sample(lin, i.uv);
    float4 b = blurTex.SampleLevel(lin, i.uv, 0);
    float coc = max(dofCoc(viewDist(i.uv), dofPlanes()), b.a);
    float f = smoothstep(0.25, 1.0, coc);
    if (dofBlurP.w > 0.5) return float4(f, f, f, sharp.a);
    return float4(lerp(sharp.rgb, b.rgb, f), sharp.a);
}
// Colour grade (baked 3D table) + vignette; alpha (the sky marker) kept.
float4 psGrade(V i) : SV_Target {
    float4 c = src.Sample(lin, i.uv);
    float n = grade.x;
    float3 g = lutTex.SampleLevel(lin, saturate(c.rgb) * ((n - 1) / n) + 0.5 / n, 0).rgb;
    float2 d = (i.uv - 0.5) * float2(grade.z, 1);
    g *= 1 - grade.y * smoothstep(0.3, 1.0, length(d) * 1.1);
    return float4(g, c.a);
}
// Sun flare + glare: same model as iw4r_mapload.dll's (volumetric.hlsl psFlare*, psGlare*). The
// visibility is the share of sky (alpha 0: occluders count as solid) around the sun, eased, + 1.
Texture2D<float> visTex : register(t5);
float4 psFlareVis(V i) : SV_Target {
    float sum = 0;
    [unroll] for (int t = 0; t < 16; t++) {
        float r = sqrt((t + 0.5) / 16);
        float a = t * 2.3999632;
        float2 u = saturate(flareSun.xy + float2(cos(a) / flareSun.z, sin(a)) * r * flareParm.w);
        sum += src.SampleLevel(lin, u, 0).a < 0.5 ? 1 : 0;
    }
    float cur = sum / 16 + 1;
    float prev = visTex.Load(int3(0, 0, 0));
    float f = prev > 0.5 ? lerp(prev, cur, flareMisc.x) : cur;
    return float4(f, f, f, 1);
}
static const float kGhostT[6] = {0.45, 0.75, 1.15, 1.4, 1.7, 2.2};
static const float kGhostR[6] = {0.035, 0.06, 0.02, 0.09, 0.045, 0.14};
static const float3 kGhostC[6] = {float3(1.0, 0.75, 0.4), float3(0.45, 0.85, 0.6), float3(0.9, 0.5, 1.0),
                                  float3(0.4, 0.6, 1.0), float3(1.0, 0.9, 0.55), float3(0.5, 0.7, 1.0)};
float4 psFlare(V i) : SV_Target {
    float vis = saturate(visTex.Load(int3(0, 0, 0)) - 1);
    float2 uv = i.uv;
    float2 asp = float2(flareSun.z, 1);
    float2 d = (uv - flareSun.xy) * asp;
    float r = length(d);
    float3 c = flareCol.rgb;
    float3 o = c * (exp(-r * r / 0.0003) * 1.5 + exp(-r / 0.05) * 0.35) * flareParm.x;
    float ang = atan2(d.y, d.x);
    float rays = pow(abs(cos(ang * 3)), 60) + 0.5 * pow(abs(cos(ang * 3 + 0.5236)), 90);
    o += c * rays * exp(-r / 0.07) * 0.35 * flareParm.x;
    o += float3(0.55, 0.72, 1.0) * c * flareParm.y * exp(-abs(d.y) / 0.0035) * exp(-abs(d.x) / 0.3);
    float2 axis = float2(0.5, 0.5) - flareSun.xy;
    [unroll] for (int t = 0; t < 6; t++) {
        float gd = length((uv - (flareSun.xy + axis * kGhostT[t])) * asp);
        float disc = saturate((kGhostR[t] - gd) / (kGhostR[t] * 0.35));
        o += kGhostC[t] * c * disc * (0.06 + 0.06 * (gd / kGhostR[t])) * flareCol.w;
    }
    float2 hc = (uv - 0.5) * asp;
    float hr = length(hc);
    float2 sd = (flareSun.xy - 0.5) * asp;
    float side = saturate(dot(hc / max(hr, 1e-4), sd / max(length(sd), 1e-4)));
    o += float3(0.6, 0.8, 1.0) * c * exp(-pow((hr - 0.42) / 0.02, 2)) * side * side * 0.06 * flareCol.w;
    o += c * flareParm.z * (0.15 + 0.85 * exp(-r / 0.35)) * 0.25;
    return float4(o * flareSun.w * vis, 0);
}
float4 psGlareDown(V i) : SV_Target {
    float3 c = src.SampleLevel(lin, i.uv, 0).rgb;
    float l = max(c.r, max(c.g, c.b));
    float k = saturate((l - glareParm.x) / max(1 - glareParm.x, 0.05));
    return float4(c * k * k, 1);
}
float4 psGlareBlur(V i) : SV_Target {
    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int t = -6; t <= 6; t++) {
        float w = exp(-t * t / 18.0);
        sum += src.SampleLevel(lin, i.uv + glareParm.zw * t, 0).rgb * w;
        wsum += w;
    }
    return float4(sum / wsum, 1);
}
float4 psGlareComp(V i) : SV_Target { return float4(src.SampleLevel(lin, i.uv, 0).rgb * glareParm.y, 0); }
// Screen-space weather: same model as iw4r_mapload.dll's (volumetric.hlsl psWeather).
float wxHash(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
float wxLayer(float3 dir, float hl, int li, float sceneDist) {
    bool snow = wxParm.x > 1.5;
    float d = wxParm.w * (li == 0 ? 0.1 : li == 1 ? 0.25 : li == 2 ? 0.5 : 1.0);
    float N = snow ? 46 + 14 * li : 96 + 22 * li;
    float aspect = snow ? 1 : 3;
    float cellW = d * 6.2831853 / N, cellH = cellW * aspect;
    float X = (atan2(dir.y, dir.x) / 6.2831853 + 0.5) * N;
    float Y = d * dir.z / hl / cellH;
    float2 tng = float2(-dir.y, dir.x) / hl;
    float fall = max(wxWind.z, 1);
    float slant = dot(wxWind.xy, tng) / fall * (cellH / cellW);
    float S = X + slant * Y;
    float Yd = Y + frac(wxParm.z * fall / (64 * cellH)) * 64;
    float pxc = wxWind.w * N / 6.2831853;
    float col = floor(S - N * floor(S / N));
    float a = 0;
    if (snow) {
        float row = floor(Yd);
        float2 key = float2(col + 17 * li, row - 64 * floor(row / 64));
        float r1 = wxHash(key), r2 = wxHash(key + 3.7), r3 = wxHash(key + 9.1);
        if (r3 < wxParm.y) {
            float sway = 0.2 * sin(wxParm.z * (1.1 + r2) + r1 * 6.28);
            float2 c = float2(floor(S) + 0.3 + 0.4 * r1 + sway, row + 0.3 + 0.4 * r2);
            float rPhys = 0.07 / (1 + 0.5 * li), rad = max(rPhys, 0.9 * pxc);
            a = saturate(1 - length(float2(S, Yd) - c) / rad) * min(1, rPhys / rad + 0.3);
        }
    } else {
        [unroll] for (int k = 0; k < 2; k++) {
            float row = floor(Yd) - k;
            float2 key = float2(col + 17 * li, row - 64 * floor(row / 64));
            float r1 = wxHash(key), r2 = wxHash(key + 3.7), r3 = wxHash(key + 9.1);
            float xc = floor(S) + 0.2 + 0.6 * r1;
            float t = (Yd - (row + r2)) / (0.55 + 0.3 * r1);
            float wPhys = 0.035 / (1 + 0.6 * li), w = max(wPhys, 0.7 * pxc);
            float across = saturate(1 - abs(S - xc) / w) * min(1, wPhys / w + 0.4);
            a = max(a, (r3 < wxParm.y && t >= 0 && t <= 1) ? across * (1 - t * 0.7) : 0);
        }
    }
    float depthFade = saturate((sceneDist - d) / (0.25 * d));
    return a * depthFade * (li == 3 ? 0.5 : li == 2 ? 0.7 : 1.0);
}
float4 psWeather(V i) : SV_Target {
    float4 c = src.Sample(lin, i.uv);
    float2 ndc = float2(i.uv.x * 2 - 1, 1 - i.uv.y * 2);
    float4 f4 = mul(invViewProj, float4(ndc, 1, 1));
    float3 dir = normalize(f4.xyz / f4.w - eye.xyz);
    float sceneDist = c.a < 0.5 ? 1e7 : viewDist(i.uv);
    float hl = length(dir.xy);
    float pole = saturate((hl - 0.3) / 0.25);
    float a = 0;
    if (pole > 0) {
        [unroll] for (int li = 0; li < 4; li++) a = 1 - (1 - a) * (1 - wxLayer(dir, hl, li, sceneDist));
    }
    a *= pole * wxColor.w;
    return float4(lerp(c.rgb, wxColor.rgb, a), c.a);
}
// Local light shafts: same model as iw4r_mapload.dll's (volumetric.hlsl psLight*).
float lCone(float3 v, float d) {
    return lSpot.w < -1.5 ? 1 : smoothstep(lSpot.w, lMisc.x, dot(v, lSpot.xyz) / max(d, 1e-3));
}
float3 lightScatter(float3 dir, float L, float2 pix) {
    float3 oc = eye.xyz - lPos.xyz;
    float R = lPos.w, R2 = R * R;
    float b = dot(oc, dir), h = b * b - (dot(oc, oc) - R2);
    if (h <= 0) return 0;
    h = sqrt(h);
    float t0 = max(-b - h, 0), t1 = min(-b + h, L);
    if (t1 <= t0) return 0;
    float dt = (t1 - t0) / 24;
    float j = frac(52.9829189 * frac(dot(pix, float2(0.06711056, 0.00583715))) + lMisc.z);
    float sum = 0;
    [unroll] for (int i = 0; i < 24; i++) {
        float3 v = oc + dir * (t0 + (i + j) * dt);
        float d2 = dot(v, v);
        float f = saturate(1 - d2 / R2);
        sum += f * f * lCone(v, sqrt(d2));
    }
    sum *= dt;
    float tm = -b;
    float3 vc = oc + dir * clamp(tm, t0, t1);
    float hh2 = max(dot(oc, oc) - b * b, 0);
    float k2 = 0.02 * R2, s = sqrt(hh2 + k2);
    float core = k2 / s * (atan((t1 - tm) / s) - atan((t0 - tm) / s));
    float fc = saturate(1 - dot(vc, vc) / R2);
    sum += 2 * core * fc * fc * lCone(vc, length(vc));
    return lCol.rgb * sum;
}
float4 psLightScatter(V i) : SV_Target {
    float2 ndc = float2(i.uv.x * 2 - 1, 1 - i.uv.y * 2);
    float4 f4 = mul(invViewProj, float4(ndc, 1, 1));
    float3 dir = normalize(f4.xyz / f4.w - eye.xyz);
    float L = src.SampleLevel(lin, i.uv, 0).a < 0.5 ? 1e7 : viewDist(i.uv);
    float3 S = lightScatter(dir, L, i.pos.xy);
    float free = L > length(lPos.xyz - eye.xyz) - 0.25 * lPos.w ? 1 : 0;
    return float4(S, free);
}
float4 psLightShaft(V i) : SV_Target {
    float4 c = src.SampleLevel(lin, i.uv, 0);
    float vis = 1;
    if (lMisc.w > 0.5) {
        float2 delta = (lScreen.xy - i.uv) * (lScreen.z / 24);
        float sum = 0, wsum = 0, w = 1;
        float2 p = i.uv;
        [unroll] for (int t = 0; t < 24; t++) {
            p += delta;
            sum += src.SampleLevel(lin, saturate(p), 0).a * w;
            wsum += w;
            w *= 0.96;
        }
        vis = lerp(1, sum / wsum, lMisc.y);
    }
    return float4(c.rgb * vis, 1);
}
float4 psLightComp(V i) : SV_Target { return float4(src.SampleLevel(lin, i.uv, 0).rgb, 0); }
float4 psSolid() : SV_Target { return float4(0, 0, 0, 1); }  // any vertex shader
)";

struct PostConstants {
    float invViewProj[16];
    float eye[4], sun[4], fogColor[4], fogLight[4], fogParm[4], sunUv[4], mask[4], glow[4], blur[4], tint[4], flags[4];
    float aoParm[4], aoFade[4], aoBlur[4], grade[4];
    float dofDist[4], dofBlurP[4], dofStep[4], dofAutoP[4];
    float flareSun[4], flareCol[4], flareParm[4], flareMisc[4], glareParm[4], wxParm[4], wxWind[4], wxColor[4];
    float lPos[4], lCol[4], lSpot[4], lMisc[4], lScreen[4];
    float csParm[4], csFade[4], viewProjM[16];
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
        for (P q : {P{"psFog", &psFog}, P{"psMask", &psMask}, P{"psBlur", &psBlur}, P{"psComposite", &psComposite}, P{"psOpaque", &psOpaque},
                    P{"psSolid", &psSolid}, P{"psAo", &psAo}, P{"psAoBlur", &psAoBlur}, P{"psGrade", &psGrade}, P{"psDofFocus", &psDofFocus},
                    P{"psDofDown", &psDofDown}, P{"psDofBlur", &psDofBlur}, P{"psDofComp", &psDofComp},
                    P{"psFlareVis", &psFlareVis}, P{"psFlare", &psFlare}, P{"psGlareDown", &psGlareDown}, P{"psGlareBlur", &psGlareBlur},
                    P{"psGlareComp", &psGlareComp}, P{"psWeather", &psWeather},
                    P{"psContact", &psContact}, P{"psLightScatter", &psLightScatter}, P{"psLightShaft", &psLightShaft}, P{"psLightComp", &psLightComp}}) {
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
        // AO: frame * ao (colour only; alpha is the sky marker).
        D3D11_BLEND_DESC mb{};
        mb.RenderTarget[0].BlendEnable = TRUE;
        mb.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
        mb.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
        mb.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        mb.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        mb.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        mb.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        mb.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        dev->CreateBlendState(&mb, &blendMultiply);
        D3D11_BLEND_DESC co{};
        co.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        dev->CreateBlendState(&co, &blendColorOnly);
        // Flare + glare: added on, colour only.
        D3D11_BLEND_DESC ac = mb;
        ac.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        ac.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        ac.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        dev->CreateBlendState(&ac, &blendAddColor);
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
        // Patches (read-only): tessellated as compiled, lit by the preview bake on worldspawn.
        if (showBrushes && !ent.rawBlocks.empty()) {
            const patch::TexSizeFn texSize = [&](const std::string& m, double& w, double& h) {
                matdef::Def def;
                int iw, ih;
                if (assets && assets->materialDef(m, def) && matdef::colorMapSize(def, iw, ih)) w = iw, h = ih;
            };
            for (const patch::Patch& p : patch::entityPatches(ent)) {
                if (!showToolFaces && gfx::isToolMaterial(p.material)) continue;
                const patch::Grid g = patch::tessellate(p, texSize);
                for (const Face& f : patch::faces(g, p.material)) {
                    auto& out = byMaterial[f.material];
                    const light::FaceMap* lm = (e == 0 && lighting) ? lighting->find(f) : nullptr;
                    for (size_t i = 1; i + 1 < f.winding.size(); i++)
                        for (size_t idx : {size_t(0), i, i + 1}) {
                            Vertex vx = vertex(f.winding[idx], f.vnormals[idx], f.uvs[2 * idx], f.uvs[2 * idx + 1], 1, 1, 1);
                            if (lm) lighting->lmapCoord(*lm, f.winding[idx], vx.lm[0], vx.lm[1]);
                            out.push_back(vx);
                        }
                }
                // Outline: the tessellated grid's border.
                auto edge = [&](int r0, int c0, int r1, int c1) { addLine(g.p[g.at(r0, c0)], g.p[g.at(r1, c1)], 0.3f, 0.6f, 1.0f); };
                for (int c = 0; c + 1 < g.w; c++) edge(0, c, 0, c + 1), edge(g.h - 1, c, g.h - 1, c + 1);
                for (int r = 0; r + 1 < g.h; r++) edge(r, 0, r + 1, 0), edge(r, g.w - 1, r + 1, g.w - 1);
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
    sceneCopy.Reset(); sceneCopySrv.Reset(); lacc.Reset(); laccRtv.Reset(); laccSrv.Reset();
    for (int i = 0; i < 2; i++) half[i].Reset(), halfRtv[i].Reset(), halfSrv[i].Reset(), aoTex[i].Reset(), aoRtv[i].Reset(), aoSrv[i].Reset();
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
        if (FAILED(dev->CreateTexture2D(&hd, nullptr, &lacc))) return false;  // local light shafts accumulator
        dev->CreateRenderTargetView(lacc.Get(), nullptr, &laccRtv);
        dev->CreateShaderResourceView(lacc.Get(), nullptr, &laccSrv);
    }
    {  // full-res AO + blur ping-pong
        D3D11_TEXTURE2D_DESC ad = td;
        ad.Format = DXGI_FORMAT_R8_UNORM;
        ad.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (int i = 0; i < 2; i++) {
            if (FAILED(dev->CreateTexture2D(&ad, nullptr, &aoTex[i]))) return false;
            dev->CreateRenderTargetView(aoTex[i].Get(), nullptr, &aoRtv[i]);
            dev->CreateShaderResourceView(aoTex[i].Get(), nullptr, &aoSrv[i]);
        }
    }
    if (!focusTex[0]) {  // 1x1 auto-focus distance, ping-pong (size independent)
        D3D11_TEXTURE2D_DESC fd = td;
        fd.Width = fd.Height = 1;
        fd.Format = DXGI_FORMAT_R32_FLOAT;
        fd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (int i = 0; i < 2; i++) {
            if (FAILED(dev->CreateTexture2D(&fd, nullptr, &focusTex[i])) || FAILED(dev->CreateTexture2D(&fd, nullptr, &visTex[i]))) return false;
            dev->CreateRenderTargetView(focusTex[i].Get(), nullptr, &focusRtv[i]);
            dev->CreateShaderResourceView(focusTex[i].Get(), nullptr, &focusSrv[i]);
            dev->CreateRenderTargetView(visTex[i].Get(), nullptr, &visRtv[i]);
            dev->CreateShaderResourceView(visTex[i].Get(), nullptr, &visSrv[i]);
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
    projScale[0] = xs, projScale[1] = ys;
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
    const bool grading = v.grade && v.gradeLut && v.debug == 0 && ensureLut(v, cam.pos);
    const bool dof = v.dof && (v.debug == 0 || v.debug == 5);
    float flareFade = 0;
    if (clip[3] > 0.02f) {
        const float nx = su * 2 - 1, ny = 1 - sv * 2, off = std::max(std::fabs(nx), std::fabs(ny));
        flareFade = std::min(1.0f, clip[3] / 0.35f) * std::max(0.0f, std::min(1.0f, (1.15f - off) / 0.15f));
    }
    const bool flare = (v.flare || (v.glare && v.glareSun > 0)) && flareFade > 0 && v.debug == 0;
    const bool glare = v.glare && v.glareIntensity > 0 && v.debug == 0;
    const float shelter = v.weather ? weatherShelter(v, cam.pos) : 0.0f;
    const bool weather = v.weather && shelter > 0.001f && v.weatherIntensity > 0 && v.debug == 0;
    const bool lights = v.lights && !v.volLights.empty() && v.lightsIntensity > 0 && v.debug == 0;
    const bool contact = v.contact && v.contactIntensity > 0 && (v.debug == 0 || v.debug == 6);
    if (!v.fog && !shafts && !v.ssao && !grading && !dof && !flare && !glare && !weather && !lights && !contact) return;
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
    pc.aoParm[0] = static_cast<float>(v.ssaoRadius);
    pc.aoParm[1] = static_cast<float>(v.ssaoIntensity);
    pc.aoParm[2] = static_cast<float>(v.ssaoBias);
    pc.aoParm[3] = v.debug == 4 ? 1.0f : 0.0f;
    pc.aoFade[0] = static_cast<float>(v.ssaoNear);
    pc.aoFade[1] = static_cast<float>(v.ssaoDistance);
    pc.aoFade[2] = 1.0f / std::max(1, viewWidth);
    pc.aoFade[3] = 1.0f / std::max(1, viewHeight);
    pc.aoBlur[2] = projScale[0];
    pc.aoBlur[3] = projScale[1];
    std::memcpy(pc.viewProjM, viewProj, sizeof pc.viewProjM);
    {
        const float cp[4] = {static_cast<float>(v.contactIntensity), static_cast<float>(v.contactThickness),
                             std::fmod(++contactFrame * 0.618034f, 1.0f), static_cast<float>(v.contactLength)};
        const float cf[4] = {static_cast<float>(v.contactNear), static_cast<float>(v.contactDistance), pc.aoFade[2], pc.aoFade[3]};
        std::memcpy(pc.csParm, cp, sizeof cp);
        std::memcpy(pc.csFade, cf, sizeof cf);
    }
    pc.grade[0] = static_cast<float>(lutSize);
    pc.grade[1] = vignetteNow;
    {
        const ULONGLONG now = GetTickCount64();
        const float dt = dofTick ? std::min(0.25f, (now - dofTick) / 1000.0f) : 0.0f;
        dofTick = now;
        const float hs = viewHeight / 1080.0f * 0.5f;
        const float d4[4] = {static_cast<float>(v.dofNearStart), static_cast<float>(v.dofNearEnd), static_cast<float>(v.dofFarStart),
                             static_cast<float>(v.dofFarEnd)};
        std::memcpy(pc.dofDist, d4, sizeof d4);
        pc.dofBlurP[0] = static_cast<float>(v.dofNearBlur) * hs;
        pc.dofBlurP[1] = static_cast<float>(v.dofFarBlur) * hs;
        pc.dofBlurP[2] = static_cast<float>(v.dofWeapon);
        pc.dofBlurP[3] = v.debug == 5 ? 1.0f : 0.0f;
        pc.dofStep[0] = 1.0f / std::max(1, viewWidth / 2), pc.dofStep[1] = 1.0f / std::max(1, viewHeight / 2);
        pc.dofStep[2] = 1.0f / std::max(1, viewWidth), pc.dofStep[3] = 1.0f / std::max(1, viewHeight);
        pc.dofAutoP[0] = v.dofAuto ? 1.0f : 0.0f;
        pc.dofAutoP[1] = 1.0f - std::exp(-static_cast<float>(v.dofAutoSpeed) * dt);
        pc.dofAutoP[2] = static_cast<float>(std::max(v.dofFarEnd * 4, 20000.0));
    }
    pc.grade[2] = static_cast<float>(aspect());
    {
        const ULONGLONG now = GetTickCount64();
        const float dt = flareTick ? std::min(0.25f, (now - flareTick) / 1000.0f) : 0.0f;
        flareTick = now;
        const float on = v.flare ? 1.0f : 0.0f;
        const float fi = static_cast<float>(v.flareIntensity);
        const float fs[4] = {su, sv, static_cast<float>(aspect()), flareFade};
        const float fc[4] = {static_cast<float>(v.flareColor.x) * fi, static_cast<float>(v.flareColor.y) * fi,
                             static_cast<float>(v.flareColor.z) * fi, static_cast<float>(v.flareGhosts) * on};
        const float fp[4] = {static_cast<float>(v.flareHalo) * on, static_cast<float>(v.flareStreak) * on,
                             v.glare ? static_cast<float>(v.glareSun) / std::max(0.01f, fi) : 0.0f, 0.012f};
        std::memcpy(pc.flareSun, fs, sizeof fs);
        std::memcpy(pc.flareCol, fc, sizeof fc);
        std::memcpy(pc.flareParm, fp, sizeof fp);
        pc.flareMisc[0] = 1.0f - std::exp(-14.0f * dt);
        pc.glareParm[0] = static_cast<float>(v.glareThreshold);
        pc.glareParm[1] = static_cast<float>(v.glareIntensity);
        const bool snow = v.weather == 2;
        pc.wxParm[0] = static_cast<float>(v.weather);
        pc.wxParm[1] = std::min(1.0f, (snow ? 0.45f : 0.5f) * static_cast<float>(v.weatherIntensity));
        pc.wxParm[2] = static_cast<float>(static_cast<double>(now % 3600000ull) / 1000.0);
        pc.wxParm[3] = static_cast<float>(v.weatherDistance);
        pc.wxWind[0] = static_cast<float>(v.weatherWind[0]);
        pc.wxWind[1] = static_cast<float>(v.weatherWind[1]);
        pc.wxWind[2] = (snow ? 48.0f : 420.0f) * static_cast<float>(v.weatherSpeed);
        pc.wxWind[3] = 2.0f / (projScale[1] * std::max(1, viewHeight));
        pc.wxColor[0] = static_cast<float>(v.weatherColor.x);
        pc.wxColor[1] = static_cast<float>(v.weatherColor.y);
        pc.wxColor[2] = static_cast<float>(v.weatherColor.z);
        pc.wxColor[3] = static_cast<float>(v.weatherOpacity * std::min(1.0, v.weatherIntensity)) * shelter;
    }
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
    // Same order as the game: AO -> fog -> shafts -> grade; each reads a fresh copy of the frame.
    if (v.ssao) {
        pass(aoRtv[0].Get(), full, psAo.Get(), sceneCopySrv.Get(), depthSrv.Get(), nullptr);
        pc.aoBlur[0] = 2.0f / std::max(1, viewWidth), pc.aoBlur[1] = 0;
        upload();
        pass(aoRtv[1].Get(), full, psAoBlur.Get(), aoSrv[0].Get(), depthSrv.Get(), nullptr);
        pc.aoBlur[0] = 0, pc.aoBlur[1] = 2.0f / std::max(1, viewHeight);
        upload();
        pass(colorRtv.Get(), full, psAoBlur.Get(), aoSrv[1].Get(), depthSrv.Get(), v.debug == 4 ? blendColorOnly.Get() : blendMultiply.Get());
        if (v.debug == 4) {
            ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            ctx->RSSetViewports(1, &full);
            return;
        }
        ctx->CopyResource(sceneCopy.Get(), color.Get());
    }
    if (contact) {  // march towards the sun -> blur across, down (depth-aware) -> multiplied in
        pass(aoRtv[0].Get(), full, psContact.Get(), sceneCopySrv.Get(), depthSrv.Get(), nullptr);
        pc.aoBlur[0] = 1.5f / std::max(1, viewWidth), pc.aoBlur[1] = 0;
        upload();
        pass(aoRtv[1].Get(), full, psAoBlur.Get(), aoSrv[0].Get(), depthSrv.Get(), nullptr);
        pc.aoBlur[0] = 0, pc.aoBlur[1] = 1.5f / std::max(1, viewHeight);
        upload();
        pass(colorRtv.Get(), full, psAoBlur.Get(), aoSrv[1].Get(), depthSrv.Get(), v.debug == 6 ? blendColorOnly.Get() : blendMultiply.Get());
        if (v.debug == 6) {
            ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            ctx->RSSetViewports(1, &full);
            return;
        }
        ctx->CopyResource(sceneCopy.Get(), color.Get());
    }
    if (v.fog) pass(colorRtv.Get(), full, psFog.Get(), sceneCopySrv.Get(), depthSrv.Get(), nullptr);
    if (dof) {  // auto focus (1x1) -> half res colour + blur size -> gather -> composite
        if (v.fog) ctx->CopyResource(sceneCopy.Get(), color.Get());
        auto bindExtra = [&](UINT slot, ID3D11ShaderResourceView* srv) { ctx->PSSetShaderResources(slot, 1, &srv); };
        const D3D11_VIEWPORT one{0, 0, 1, 1, 0, 1};
        if (v.dofAuto) {
            const int prev = focusCur, cur = 1 - focusCur;
            if (!focusValid) {
                const float zero[4] = {0, 0, 0, 0};
                ctx->ClearRenderTargetView(focusRtv[prev].Get(), zero);
            }
            pass(focusRtv[cur].Get(), one, psDofFocus.Get(), focusSrv[prev].Get(), depthSrv.Get(), nullptr);
            focusCur = cur;
            focusValid = true;
        }
        bindExtra(3, focusSrv[focusCur].Get());
        pass(halfRtv[0].Get(), halfVp, psDofDown.Get(), sceneCopySrv.Get(), depthSrv.Get(), nullptr);
        bindExtra(3, focusSrv[focusCur].Get());
        pass(halfRtv[1].Get(), halfVp, psDofBlur.Get(), halfSrv[0].Get(), depthSrv.Get(), nullptr);
        bindExtra(3, focusSrv[focusCur].Get());
        bindExtra(4, halfSrv[1].Get());
        pass(colorRtv.Get(), full, psDofComp.Get(), sceneCopySrv.Get(), depthSrv.Get(), nullptr);
        bindExtra(3, nullptr);
        bindExtra(4, nullptr);
        if (v.debug == 5) {
            ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            ctx->RSSetViewports(1, &full);
            return;
        }
        ctx->CopyResource(sceneCopy.Get(), color.Get());
    }
    if (lights) {  // per light: scatter (half res) -> shadowed towards the light into the accumulator -> added on
        if (v.fog && !dof) ctx->CopyResource(sceneCopy.Get(), color.Get());
        struct Pick {
            size_t i;
            float score, clip[4];
        };
        std::vector<Pick> picks;
        for (size_t i = 0; i < v.volLights.size(); i++) {
            const light::VolLight& L = v.volLights[i];
            const Vec3 d = L.origin - cam.pos;
            const double dist = length(d);
            if (dist - L.radius > v.lightsDistance) continue;
            Pick pk{i, 0, {0, 0, 0, 0}};
            const float p[3] = {static_cast<float>(L.origin.x), static_cast<float>(L.origin.y), static_cast<float>(L.origin.z)};
            for (int r = 0; r < 4; r++) pk.clip[r] = viewProj[r * 4] * p[0] + viewProj[r * 4 + 1] * p[1] + viewProj[r * 4 + 2] * p[2] + viewProj[r * 4 + 3];
            if (pk.clip[3] < -L.radius) continue;
            if (pk.clip[3] > 0 && dist > L.radius) {
                const float slack = 1 + static_cast<float>(L.radius) / std::max(pk.clip[3], 1.0f) * std::max(projScale[0], projScale[1]);
                if (std::fabs(pk.clip[0]) / pk.clip[3] > slack || std::fabs(pk.clip[1]) / pk.clip[3] > slack) continue;
            }
            const double lum = L.color.x * 0.3 + L.color.y * 0.59 + L.color.z * 0.11;
            pk.score = static_cast<float>(lum * L.radius * L.radius / std::max(dist * dist, L.radius * L.radius));
            picks.push_back(pk);
        }
        std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.score > b.score; });
        if (picks.size() > static_cast<size_t>(v.lightsMax)) picks.resize(v.lightsMax);
        const float zero[4] = {0, 0, 0, 0};
        ctx->ClearRenderTargetView(laccRtv.Get(), zero);
        const double secs = static_cast<double>(GetTickCount64() % 3600000ull) / 1000.0;
        lightFrame++;
        for (const Pick& pk : picks) {
            const light::VolLight& L = v.volLights[pk.i];
            float k = 0.0007f * static_cast<float>(v.lightsIntensity);
            if (L.flicker > 0) {
                const double ph = pk.i * 1.7;
                const double n = 0.5 * std::sin(secs * 13.0 + ph) + 0.3 * std::sin(secs * 7.3 + 2 * ph) + 0.2 * std::sin(secs * 23.0 + 3 * ph);
                k *= static_cast<float>(1 - L.flicker * (0.5 + 0.5 * n));
            }
            const bool front = pk.clip[3] > 1;
            const float lp[4] = {static_cast<float>(L.origin.x), static_cast<float>(L.origin.y), static_cast<float>(L.origin.z), static_cast<float>(L.radius)};
            const float lc[4] = {static_cast<float>(L.color.x) * k, static_cast<float>(L.color.y) * k, static_cast<float>(L.color.z) * k, 0};
            const float ls[4] = {static_cast<float>(L.spotDir.x), static_cast<float>(L.spotDir.y), static_cast<float>(L.spotDir.z),
                                 L.spot ? static_cast<float>(L.cosOuter) : -2.0f};
            const float lm[4] = {L.spot ? static_cast<float>(L.cosInner) : -2.0f, static_cast<float>(v.lightsShadows),
                                 std::fmod(lightFrame * 0.618034f, 1.0f), front && v.lightsShadows > 0 ? 1.0f : 0.0f};
            const float lscr[4] = {front ? 0.5f + 0.5f * pk.clip[0] / pk.clip[3] : 0.5f, front ? 0.5f - 0.5f * pk.clip[1] / pk.clip[3] : 0.5f, 0.9f, 0};
            std::memcpy(pc.lPos, lp, sizeof lp);
            std::memcpy(pc.lCol, lc, sizeof lc);
            std::memcpy(pc.lSpot, ls, sizeof ls);
            std::memcpy(pc.lMisc, lm, sizeof lm);
            std::memcpy(pc.lScreen, lscr, sizeof lscr);
            upload();
            pass(halfRtv[0].Get(), halfVp, psLightScatter.Get(), sceneCopySrv.Get(), depthSrv.Get(), nullptr);
            pass(laccRtv.Get(), halfVp, psLightShaft.Get(), halfSrv[0].Get(), nullptr, blendAddColor.Get());
        }
        if (!picks.empty()) {
            pass(colorRtv.Get(), full, psLightComp.Get(), laccSrv.Get(), nullptr, blendAddColor.Get());
            ctx->CopyResource(sceneCopy.Get(), color.Get());
        }
    }
    if (weather) {
        if (v.fog && !dof && !lights) ctx->CopyResource(sceneCopy.Get(), color.Get());
        pass(colorRtv.Get(), full, psWeather.Get(), sceneCopySrv.Get(), depthSrv.Get(), nullptr);
        ctx->CopyResource(sceneCopy.Get(), color.Get());
    }
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
    if (glare) {  // bright pass (half res) -> blur across -> down -> added on
        ctx->CopyResource(sceneCopy.Get(), color.Get());
        const float step = static_cast<float>(v.glareRadius) * (viewHeight / 1080.0f) * 0.5f / 6;
        pass(halfRtv[0].Get(), halfVp, psGlareDown.Get(), sceneCopySrv.Get(), nullptr, nullptr);
        pc.glareParm[2] = step / std::max(1, viewWidth / 2), pc.glareParm[3] = 0;
        upload();
        pass(halfRtv[1].Get(), halfVp, psGlareBlur.Get(), halfSrv[0].Get(), nullptr, nullptr);
        pc.glareParm[2] = 0, pc.glareParm[3] = step / std::max(1, viewHeight / 2);
        upload();
        pass(halfRtv[0].Get(), halfVp, psGlareBlur.Get(), halfSrv[1].Get(), nullptr, nullptr);
        pass(colorRtv.Get(), full, psGlareComp.Get(), halfSrv[0].Get(), nullptr, blendAddColor.Get());
    }
    if (flare) {  // visibility around the sun (1x1, eased) -> flare added on
        ctx->CopyResource(sceneCopy.Get(), color.Get());
        const int prev = visCur, cur = 1 - visCur;
        if (!visValid) {
            const float zero[4] = {0, 0, 0, 0};
            ctx->ClearRenderTargetView(visRtv[prev].Get(), zero);
        }
        ID3D11ShaderResourceView* pv = visSrv[prev].Get();
        ctx->PSSetShaderResources(5, 1, &pv);
        const D3D11_VIEWPORT one{0, 0, 1, 1, 0, 1};
        pass(visRtv[cur].Get(), one, psFlareVis.Get(), sceneCopySrv.Get(), nullptr, nullptr);
        visCur = cur;
        visValid = true;
        ID3D11ShaderResourceView* cv = visSrv[cur].Get();
        ctx->PSSetShaderResources(5, 1, &cv);
        pass(colorRtv.Get(), full, psFlare.Get(), nullptr, nullptr, blendAddColor.Get());
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(5, 1, &none);
    }
    if (grading) {
        ctx->CopyResource(sceneCopy.Get(), color.Get());
        ID3D11ShaderResourceView* lut = lutSrv.Get();
        ctx->PSSetShaderResources(2, 1, &lut);
        pass(colorRtv.Get(), full, psGrade.Get(), sceneCopySrv.Get(), nullptr, nullptr);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(2, 1, &none);
    }
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->RSSetViewports(1, &full);
}

// How much weather reaches the camera: trigger_weather volumes blended over 1 in order, eased.
float Renderer::weatherShelter(const light::Volumetric& v, const Vec3& eyePos) {
    const size_t nv = v.weatherVolumes.size();
    const ULONGLONG now = GetTickCount64();
    const bool newSet = wxW.size() != nv;
    const float dt = newSet || !wxTick ? 0.0f : std::min(0.25f, (now - wxTick) / 1000.0f);
    wxTick = now;
    if (newSet) wxW.assign(nv, 0.0f);
    const float eye[3] = {static_cast<float>(eyePos.x), static_cast<float>(eyePos.y), static_cast<float>(eyePos.z)};
    float m = 1;
    for (size_t i = 0; i < nv; i++) {
        const light::WeatherVolume& wv = v.weatherVolumes[i];
        const float target = gradevol::targetWeight(wv.brushes, static_cast<float>(wv.blend), eye);
        wxW[i] = newSet ? target : gradevol::approach(wxW[i], target, static_cast<float>(wv.blendTime), dt);
        m += (static_cast<float>(wv.intensity) - m) * wxW[i];
    }
    return m;
}

// The frame's grade table: the base with each trigger_grade volume blended over it by how far the
// camera is inside (eased over its blend time); re-uploaded only when the weights move.
bool Renderer::ensureLut(const light::Volumetric& v, const Vec3& eyePos) {
    const light::GradeLut& t = *v.gradeLut;
    if (t.size < 2) return false;
    const size_t nv = v.gradeVolumes.size();
    const ULONGLONG now = GetTickCount64();
    const bool newSet = lutUploaded != &t || volW.size() != nv || lutSize != t.size;
    if (newSet) {
        volW.assign(nv, 0.0f);
        volWUploaded.assign(nv, -1.0f);
        gradeTick = now;
    }
    const float dt = std::min(0.25f, (now - gradeTick) / 1000.0f);
    gradeTick = now;
    const float eye[3] = {static_cast<float>(eyePos.x), static_cast<float>(eyePos.y), static_cast<float>(eyePos.z)};
    bool changed = newSet || !lutSrv;
    for (size_t i = 0; i < nv; i++) {
        const light::GradeVolume& gv = v.gradeVolumes[i];
        const float target = gradevol::targetWeight(gv.brushes, static_cast<float>(gv.blend), eye);
        volW[i] = newSet ? target : gradevol::approach(volW[i], target, static_cast<float>(gv.blendTime), dt);
        if (std::fabs(volW[i] - volWUploaded[i]) > 1.0f / 512 || (volW[i] == 0) != (volWUploaded[i] == 0)) changed = true;
    }
    if (!changed) return true;
    std::vector<uint8_t> rgb(t.rgb);
    float vig = static_cast<float>(v.vignette);
    for (size_t i = 0; i < nv; i++) {
        const light::GradeVolume& gv = v.gradeVolumes[i];
        if (volW[i] <= 0 || !gv.lut || gv.lut->size != t.size) continue;
        gradevol::blendInto(rgb, gv.lut->rgb, volW[i]);
        vig += (static_cast<float>(gv.vignette) - vig) * volW[i];
    }
    std::vector<uint8_t> rgba(static_cast<size_t>(t.size) * t.size * t.size * 4, 255);
    for (size_t i = 0, n = rgba.size() / 4; i < n; i++)
        for (int k = 0; k < 3; k++) rgba[i * 4 + k] = rgb[i * 3 + k];
    if (!lutSrv || lutSize != t.size) {
        lutSrv.Reset();
        lutTex.Reset();
        D3D11_TEXTURE3D_DESC d{};
        d.Width = d.Height = d.Depth = static_cast<UINT>(t.size);
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture3D(&d, nullptr, &lutTex)) || FAILED(dev->CreateShaderResourceView(lutTex.Get(), nullptr, &lutSrv))) {
            lutSrv.Reset();
            return false;
        }
    }
    ctx->UpdateSubresource(lutTex.Get(), 0, nullptr, rgba.data(), static_cast<UINT>(t.size * 4), static_cast<UINT>(t.size * t.size * 4));
    lutUploaded = &t;
    lutSize = t.size;
    volWUploaded = volW;
    vignetteNow = vig;
    return true;
}
