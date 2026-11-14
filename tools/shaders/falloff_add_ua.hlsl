// iw4-radiant's own shader pair for "falloff + uv-anim + additive" world materials (stock
// wc_unlit_falloff_add_lin_ua, e.g. wc/atmos_particles_dust). Mirrors the stock vertex shader
// (vertcol_simple_fog_ua_foa.hlsl, disassembled from trainer.ff) register for register, so the
// stock pass arguments drive it: c0 viewProjectionMatrix, c4 worldMatrix, c13 falloffParms,
// c14 falloffBeginColor, c15 falloffEndColor, c20 uvAnimParms, c21 fogConsts, c22 gameTime,
// s0 colorMapSampler. Compiled to vs_3_0 / ps_3_0 by tools/shaders/build.py into
// src/zone/effectshaders.h.

float4x4 viewProjectionMatrix : register(c0);
float4x4 worldMatrix : register(c4);
float4 falloffParms : register(c13);
float4 falloffBeginColor : register(c14);
float4 falloffEndColor : register(c15);
float4 uvAnimParms : register(c20);
float4 fogConsts : register(c21);
float4 gameTime : register(c22);
sampler2D colorMapSampler : register(s0);

struct VsIn {
    float4 pos : POSITION;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float4 normal : NORMAL;  // packed unit vector, raw bytes 0..255 (w = scale)
};
struct VsOut {
    float4 pos : POSITION;
    float4 color : COLOR0;
    float3 uvFog : TEXCOORD0;  // xy: animated uv, z: fog visibility
};

VsOut vsMain(VsIn i) {
    VsOut o;
    float4 world = mul(float4(i.pos.xyz, 1), worldMatrix);  // camera-relative
    o.pos = mul(world, viewProjectionMatrix);

    // uvAnimParms: xy scroll per second, z rotation (radians per second) about the uv centre.
    float t = gameTime.w;
    float s, c;
    sincos(uvAnimParms.z * t, s, c);
    float2 d = i.uv - 0.5;
    o.uvFog.xy = float2(c * d.x - s * d.y, s * d.x + c * d.y) + 0.5 + uvAnimParms.xy * t;

    // Falloff: fade by how face-on the surface is to the eye (falloffParms.zw scale/bias the
    // squared cosine), tinting from falloffEndColor (edge-on) to falloffBeginColor (face-on).
    float3 n = (i.normal.xyz * (1.0 / 127.0) - 1.0) * (i.normal.w * (1.0 / 255.0) + 0.752941191);
    float3 wn = mul(n, (float3x3)worldMatrix);
    float3 eye = normalize(world.xyz);
    float k = dot(eye, wn);
    float f = saturate(k * k * falloffParms.z + falloffParms.w);
    o.color.rgb = f * i.color.rgb * lerp(falloffEndColor.rgb, falloffBeginColor.rgb, f);
    o.color.a = i.color.a;

    o.uvFog.z = saturate(max(exp(length(world.xyz) * fogConsts.z + fogConsts.w), fogConsts.y));
    return o;
}

// Additive, in linear space (the texture and colour are squared like the stock _lin shaders);
// fades to black in fog, so it adds nothing far away.
float4 psMain(float4 color : COLOR0, float3 uvFog : TEXCOORD0) : COLOR {
    float4 t = tex2D(colorMapSampler, uvFog.xy);
    float3 rgb = t.rgb * color.rgb;
    return float4(rgb * rgb * (t.a * color.a * uvFog.z), 1);
}
