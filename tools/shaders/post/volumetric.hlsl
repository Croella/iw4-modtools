// Screen-space volumetric light scattering (sun shafts) for iw4r_mapload.dll's post pass.
// GPU Gems 3 ch. 13 style: (1) mask = sky pixels (from the game's floatz depth target, or a
// brightness threshold when depth isn't available) plus a glow around the sun, at half resolution;
// (2) two radial-blur passes towards the sun's screen position with exponential decay; (3) the
// result is added onto the frame. Compiled by tools/shaders/build_post.py -> src/dll/postshaders.h.

float4 halfPixel : register(c0);  // xy: -1/w, +1/h of the target (D3D9 pixel-centre offset)

struct VsOut {
    float4 pos : POSITION;
    float2 uv : TEXCOORD0;
};

// Input: xy = clip position, zw = uv.
VsOut vsQuad(float4 v : POSITION) {
    VsOut o;
    o.pos = float4(v.xy + halfPixel.xy, 0, 1);
    o.uv = v.zw;
    return o;
}

sampler2D src0 : register(s0);  // scene copy / previous pass
sampler2D src1 : register(s1);  // floatz depth

float4 sun : register(c1);      // xy: sun uv, z: aspect (w/h), w: glow radius (uv units)
float4 mask : register(c2);     // x: sky test (0 brightness, 1 floatz, 2 INTZ), y: sky depth threshold, z: brightness threshold, w: sky weight
float4 glow : register(c3);     // rgb: glow colour * strength
float4 blur : register(c4);     // x: step scale (fraction of the way to the sun per pass), y: decay, z: weight
float4 tint : register(c5);     // rgb: colour * intensity, w: fade
float4 dbg : register(c6);      // x: depth display scale, y: P[10], z: P[14] (INTZ -> view z)

float lum(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float4 psMask(float2 uv : TEXCOORD0) : COLOR {
    float3 c = tex2D(src0, uv).rgb;
    float sky;
    if (mask.x > 1.5) {  // hardware depth: the sky is at (or past) the far end; view z = P14 / (d - P10)
        float z = tex2D(src1, uv).r;
        float den = z - dbg.y;
        sky = (z >= 0.99999 || den >= -1e-7 || dbg.z / den >= mask.y) ? 1 : 0;
    } else if (mask.x > 0.5) {
        float z = tex2D(src1, uv).r;
        sky = (z <= 0 || z >= mask.y) ? 1 : 0;
    } else {
        sky = lum(c) > mask.z ? 1 : 0;
    }
    float2 d = (uv - sun.xy) * float2(sun.z, 1);
    float g = exp(-dot(d, d) / (sun.w * sun.w));
    float3 m = sky * (max(c - mask.z, 0) * mask.w + g * glow.rgb);
    return float4(m, 1);
}

#define TAPS 32
float4 psBlur(float2 uv : TEXCOORD0) : COLOR {
    float2 delta = (uv - sun.xy) * (blur.x / TAPS);
    float3 sum = 0;
    float illum = 1, wsum = 0;
    float2 p = uv;
    [unroll] for (int i = 0; i < TAPS; i++) {
        sum += tex2D(src0, p).rgb * illum;
        wsum += illum;
        illum *= blur.y;
        p -= delta;
    }
    return float4(sum * blur.z / wsum, 1);
}

float4 psComposite(float2 uv : TEXCOORD0) : COLOR {
    return float4(tex2D(src0, uv).rgb * tint.rgb * tint.w, 1);
}

// Debug views (volumetric debug=2): the depth target as grey (white = sky test passes).
float4 psDebugDepth(float2 uv : TEXCOORD0) : COLOR {
    float z = tex2D(src1, uv).r;
    float sky = (z <= 0 || z >= mask.y) ? 1 : 0;
    float g = saturate(z * dbg.x);
    return float4(sky ? float3(1, 0.85, 0.3) : float3(g, g, g), 1);
}

// ---- Height fog --------------------------------------------------------------------------------
// Per pixel: world position from depth + a view ray (the ray is linear in screen uv and scaled so
// its view-space z is 1), then the analytic optical depth of an exponential height fog along it,
// lit by an ambient colour plus Henyey-Greenstein forward scattering of the sun.
sampler2D depthTex : register(s2);  // INTZ (hardware depth, 0..1) or floatz (view z)

float4 fogDepth : register(c7);   // x: P[10], y: P[14], z: mode (1 INTZ, 2 floatz), w: distance used for sky
float4 fogRay0 : register(c8);    // xyz: ray at uv (0,0); w: floatz sky threshold
float4 fogRayX : register(c9);    // xyz: d(ray)/du
float4 fogRayY : register(c10);   // xyz: d(ray)/dv
float4 fogEye : register(c11);    // xyz: camera origin, w: fog start distance
float4 fogSun : register(c12);    // xyz: unit vector to the sun, w: anisotropy g
float4 fogColor : register(c13);  // rgb: ambient fog colour, w: max opacity
float4 fogLight : register(c14);  // rgb: sun scattering colour, w: sky multiplier
float4 fogParm : register(c15);   // x: density at base height, y: height falloff (1/units), z: base height, w: debug

float fogAmount(float2 uv, out float3 dir) {
    float3 ray = fogRay0.xyz + uv.x * fogRayX.xyz + uv.y * fogRayY.xyz;
    float d = tex2D(depthTex, uv).r;
    bool sky;
    float vz;
    if (fogDepth.z < 1.5) {  // INTZ: z_ndc = P10 + P14 / vz (standard, infinite or reversed projections)
        float den = d - fogDepth.x;
        vz = fogDepth.y / (abs(den) > 1e-7 ? den : -1e-7);
        sky = d >= 0.99999 || !(vz > 0 && vz < 1e7);
    } else {
        sky = d <= 0 || d >= fogRay0.w;
        vz = d;
    }
    if (sky) vz = fogDepth.w;
    float rl = length(ray);
    dir = ray / rl;
    float L = max(vz * rl, 0);
    float Lf = max(L - fogEye.w, 0);
    float b = fogParm.y;
    float k = b * dir.z * Lf;
    float shape = abs(k) > 1e-4 ? (1 - exp(-k)) / k : 1;
    float od = fogParm.x * exp(-b * (fogEye.z - fogParm.z)) * Lf * shape;
    float a = min(1 - exp(-od), fogColor.w);
    return sky ? a * fogLight.w : a;
}

float4 psFog(float2 uv : TEXCOORD0) : COLOR {
    float3 dir;
    float a = fogAmount(uv, dir);
    float g = fogSun.w;
    float c = dot(dir, fogSun.xyz);
    float hg = (1 - g * g) / pow(max(1 + g * g - 2 * g * c, 1e-4), 1.5);  // x 4pi: isotropic = 1
    float3 light = fogColor.rgb + fogLight.rgb * hg;
    if (fogParm.w > 0.5) return float4(a, a, a, 1);
    float3 scene = tex2D(src0, uv).rgb;
    return float4(lerp(scene, light, a), 1);
}

// ---- Sun occluders ("occluder" triangles: full_clip brushes) -----------------------------------
// Invisible in the game, so they're drawn black into the shaft mask. World-space positions through
// the frame's view-projection (c1..c4 = its columns), less occEye (the matrices are camera-relative).
float4 occCol0 : register(c1);
float4 occCol1 : register(c2);
float4 occCol2 : register(c3);
float4 occCol3 : register(c4);
float4 occEye : register(c5);

float4 vsOccluder(float4 v : POSITION) : POSITION {
    float4 p = float4(v.xyz - occEye.xyz, 1);
    float4 o = float4(dot(p, occCol0), dot(p, occCol1), dot(p, occCol2), dot(p, occCol3));
    o.xy += halfPixel.xy * o.w;
    return o;
}

float4 psBlack() : COLOR { return float4(0, 0, 0, 1); }
