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

// ---- Ambient occlusion (SSAO) ------------------------------------------------------------------
// View-space positions from the depth buffer (INTZ: z_ndc = P10 + P14 / vz; floatz: vz directly)
// and the projection's x/y scale, a normal from the neighbouring depths, then 16 spiral taps in a
// world-space radius around the pixel: occlusion = how far each tap rises above the surface's
// tangent plane (cosine, less a bias) faded out towards the radius. Noisy per pixel (rotated
// spiral), so two depth-aware blur passes follow; the last one multiplies into the frame.
sampler2D aoDepth : register(s2);  // INTZ or floatz (same texture as the fog's)

float4 aoProj : register(c16);    // x: P[0], y: P[5], z: P[8], w: P[9]
float4 aoDepthP : register(c17);  // x: P[10], y: P[14], z: mode (1 INTZ, 2 floatz), w: floatz sky threshold
float4 aoParm : register(c18);    // x: radius (world units), y: intensity, z: bias, w: 1 = show AO only
float4 aoFade : register(c19);    // x: near distance, y: far distance, z: 1/width, w: 1/height
float4 aoBlur : register(c20);    // xy: one texel along the blur direction

float aoViewZ(float2 uv) {
    float d = tex2Dlod(aoDepth, float4(uv, 0, 0)).r;
    if (aoDepthP.z < 1.5) {
        float den = d - aoDepthP.x;
        if (d >= 0.99999 || den >= -1e-7) return 1e7;
        float z = aoDepthP.y / den;
        return z > 0 ? z : 1e7;
    }
    return (d <= 0 || d >= aoDepthP.w) ? 1e7 : d;
}

float3 aoViewPos(float2 uv, float z) {
    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    return float3((ndc.x - aoProj.z) * z / aoProj.x, (ndc.y - aoProj.w) * z / aoProj.y, z);
}

#define AO_TAPS 16
float4 psAo(float2 uv : TEXCOORD0) : COLOR {
    float z = aoViewZ(uv);
    if (z >= 1e6 || z < aoFade.x * 0.5) return float4(1, 1, 1, 1);
    float3 P = aoViewPos(uv, z);
    float2 tx = float2(aoFade.z, 0), ty = float2(0, aoFade.w);
    float3 pr = aoViewPos(uv + tx, aoViewZ(uv + tx)), pl = aoViewPos(uv - tx, aoViewZ(uv - tx));
    float3 pd = aoViewPos(uv + ty, aoViewZ(uv + ty)), pu = aoViewPos(uv - ty, aoViewZ(uv - ty));
    float3 dx = abs(pr.z - z) < abs(z - pl.z) ? pr - P : P - pl;
    float3 dy = abs(pd.z - z) < abs(z - pu.z) ? pd - P : P - pu;
    float3 n = normalize(cross(dx, dy));
    if (dot(n, P) > 0) n = -n;  // towards the camera
    float r = aoParm.x, r2 = r * r;
    // Screen radius (uv) of the world radius at this depth, capped so close-ups stay cheap.
    float2 scale = 0.5 * float2(aoProj.x, aoProj.y) * r / z;
    float s = min(1, 0.25 / max(scale.y, 1e-5));
    scale *= s;
    float2 pix = uv / aoFade.zw;
    float ang = 6.2831853 * frac(52.9829189 * frac(dot(pix, float2(0.06711056, 0.00583715))));
    float sum = 0;
    [unroll] for (int i = 0; i < AO_TAPS; i++) {
        float t = (i + 0.5) / AO_TAPS;
        float a = ang + i * 2.3999632;
        float2 q = uv + float2(cos(a), sin(a)) * t * scale;
        float3 Q = aoViewPos(q, aoViewZ(q));
        float3 v = Q - P;
        float vv = dot(v, v);
        float c = dot(v, n) * rsqrt(vv + 1e-4);
        sum += max(c - aoParm.z, 0) * saturate(1 - vv / r2);
    }
    float ao = saturate(1 - aoParm.y * 2 * sum / (AO_TAPS * (1 - aoParm.z)));
    float fade = saturate((z - aoFade.x) / max(aoFade.x * 0.5, 1)) * saturate((aoFade.y - z) / (aoFade.y * 0.25));
    ao = lerp(1, ao, fade);
    return float4(ao, ao, ao, 1);
}

// Depth-aware blur of the AO (src0), 9 taps along aoBlur.xy. Output rgb = AO (the composite
// multiplies the frame by it; aoParm.w = 1 draws it as is).
float4 psAoBlur(float2 uv : TEXCOORD0) : COLOR {
    float zc = aoViewZ(uv);
    float sum = 0, wsum = 0;
    [unroll] for (int i = -4; i <= 4; i++) {
        float2 u = uv + aoBlur.xy * i;
        float zi = aoViewZ(u);
        float w = (5 - abs(i)) * saturate(1 - abs(zi - zc) / (0.04 * zc + 2));
        sum += tex2Dlod(src0, float4(u, 0, 0)).r * w;
        wsum += w;
    }
    float ao = wsum > 0 ? sum / wsum : 1;
    return float4(ao, ao, ao, 1);
}

// ---- Contact shadows ---------------------------------------------------------------------------
// Short screen-space ray march from each pixel towards the sun through the depth buffer: anything
// standing in the way within csSun.w units (a zombie's feet on the floor, a crate on the ground,
// the player model) darkens the pixel. Catches the small shadows the engine's shadow maps miss.
// Uses the AO constants (c16 projection, c17 depth, c19 near/far/texel). Output r = the factor
// the frame is multiplied by (blurred with psAoBlur first).
float4 csSun : register(c39);   // xyz: view-space unit vector to the sun, w: march length (world units)
float4 csParm : register(c40);  // x: intensity, y: thickness (units), z: jitter, w: 1 = show the shadow only

float2 csUv(float3 Q) {
    float2 ndc = float2(Q.x * aoProj.x / Q.z + aoProj.z, Q.y * aoProj.y / Q.z + aoProj.w);
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

#define CS_STEPS 16
float4 psContact(float2 uv : TEXCOORD0) : COLOR {
    float z = aoViewZ(uv);
    if (z >= 1e6 || z < aoFade.x) return float4(1, 1, 1, 1);
    float3 P = aoViewPos(uv, z);
    float2 tx = float2(aoFade.z, 0), ty = float2(0, aoFade.w);
    float3 pr = aoViewPos(uv + tx, aoViewZ(uv + tx)), pl = aoViewPos(uv - tx, aoViewZ(uv - tx));
    float3 pd = aoViewPos(uv + ty, aoViewZ(uv + ty)), pu = aoViewPos(uv - ty, aoViewZ(uv - ty));
    float3 dx = abs(pr.z - z) < abs(z - pl.z) ? pr - P : P - pl;
    float3 dy = abs(pd.z - z) < abs(z - pu.z) ? pd - P : P - pu;
    float3 n = normalize(cross(dx, dy));
    if (dot(n, P) > 0) n = -n;
    float ndl = dot(n, csSun.xyz);
    if (ndl <= 0.02) return float4(1, 1, 1, 1);  // faces away from the sun: already in shadow
    float2 pix = uv / aoFade.zw;
    float j = frac(52.9829189 * frac(dot(pix, float2(0.06711056, 0.00583715))) + csParm.z);
    // Start a little off the surface so it doesn't shadow itself.
    float3 O = P + n * (0.5 + 0.002 * z);
    float occ = 0;
    [unroll] for (int i = 0; i < CS_STEPS; i++) {
        float t = (i + j) / CS_STEPS;
        float3 Q = O + csSun.xyz * (csSun.w * t * t + 0.5);
        if (Q.z < aoFade.x) break;
        float2 q = csUv(Q);
        if (q.x < 0 || q.x > 1 || q.y < 0 || q.y > 1) break;
        if (dot((q - uv) / aoFade.zw, (q - uv) / aoFade.zw) < 4) continue;  // the pixel's own neighbours (depth edges)
        float zs = aoViewZ(q);
        if (zs < aoFade.x) continue;  // the viewmodel casts nothing
        float dz = Q.z - zs;
        if (dz > 0.003 * Q.z + 0.3 && dz < csParm.y) {
            occ = 1 - t * t * 0.6;  // fades along the ray: contact is darkest
            break;
        }
    }
    float fade = saturate((aoFade.y - z) / (aoFade.y * 0.25)) * saturate(ndl * 4);
    float f = 1 - csParm.x * occ * fade;
    return float4(f, f, f, 1);
}

// ---- Colour grading ----------------------------------------------------------------------------
// The whole grade (artist LUT + exposure/contrast/saturation/white balance/lift-gamma-gain) is
// one 3D table baked by the compiler; plus a vignette.
sampler3D lutTex : register(s3);
float4 gradeParm : register(c21);  // x: table size, y: vignette, z: aspect (w/h)

float4 psGrade(float2 uv : TEXCOORD0) : COLOR {
    float3 c = saturate(tex2D(src0, uv).rgb);
    float n = gradeParm.x;
    float3 g = tex3D(lutTex, c * ((n - 1) / n) + 0.5 / n).rgb;
    float2 d = (uv - 0.5) * float2(gradeParm.z, 1);
    g *= 1 - gradeParm.y * smoothstep(0.3, 1.0, length(d) * 1.1);
    return float4(g, 1);
}

// ---- Depth of field ----------------------------------------------------------------------------
// IW-style planes: full near blur at/before nearStart, sharp from nearEnd to farStart, full far blur
// from farEnd (the sky counts as far). Blur sizes ("circle of confusion") are in half-res pixels.
// Auto focus: a 1x1 target eases towards the depth under the crosshair each frame and the planes
// become offsets from it. Passes: focus (1x1) -> down (half res: colour + CoC/32 in alpha) -> gather
// (disc, a farther tap can't blur over a sharper nearer pixel) -> composite (full res).
// Depth decoding uses aoDepthP (c17) like the AO.
sampler2D dofFocusTex : register(s4);
float4 dofDist : register(c22);   // nearStart, nearEnd, farStart, farEnd (offsets from the focus in auto mode)
float4 dofBlurP : register(c23);  // x: near max CoC, y: far max CoC (half-res px), z: weapon distance, w: 1 = show the blur amount
float4 dofStep : register(c24);   // xy: half-res texel (uv), zw: full-res texel (uv)
float4 dofAutoP : register(c25);  // x: 1 = auto focus, y: ease factor this frame, z: distance the sky counts as

float4 dofPlanes() {
    if (dofAutoP.x < 0.5) return dofDist;
    float f = tex2Dlod(dofFocusTex, float4(0.5, 0.5, 0, 0)).r;
    return float4(max(f - dofDist.x, 0), max(f - dofDist.y, 0), f + dofDist.z, f + dofDist.w);
}

float dofCoc(float z, float4 pl) {
    if (z < dofBlurP.z) return 0;
    float n = dofBlurP.x * saturate((pl.y - z) / max(pl.y - pl.x, 1));
    float f = dofBlurP.y * saturate((z - pl.z) / max(pl.w - pl.z, 1));
    return max(n, f);
}

float4 psDofFocus(float2 uv : TEXCOORD0) : COLOR {
    float sum = 0;
    [unroll] for (int i = 0; i < 5; i++) {
        float2 o = i == 0 ? float2(0, 0) : float2(i == 1 ? 0.02 : i == 2 ? -0.02 : 0, i == 3 ? 0.03 : i == 4 ? -0.03 : 0);
        float z = aoViewZ(float2(0.5, 0.5) + o);
        sum += z < dofBlurP.z ? dofAutoP.z : min(z, dofAutoP.z);  // the weapon / the sky: as far as the sky
    }
    float cur = sum / 5;
    float prev = tex2Dlod(src1, float4(0.5, 0.5, 0, 0)).r;
    float f = prev > 0 ? lerp(prev, cur, dofAutoP.y) : cur;
    return float4(f, f, f, 1);
}

float4 psDofDown(float2 uv : TEXCOORD0) : COLOR {
    float3 c = tex2Dlod(src0, float4(uv, 0, 0)).rgb;  // bilinear: the 2x2 average
    return float4(c, dofCoc(aoViewZ(uv), dofPlanes()) / 32);
}

#define DOF_TAPS 20
float4 psDofBlur(float2 uv : TEXCOORD0) : COLOR {
    float4 c0 = tex2Dlod(src0, float4(uv, 0, 0));
    float cc = c0.a * 32, zc = aoViewZ(uv);
    float R = max(dofBlurP.x, dofBlurP.y);
    float3 sum = c0.rgb;
    float wsum = 1, csum = cc;
    [unroll] for (int i = 0; i < DOF_TAPS; i++) {
        float t = sqrt((i + 0.5) / DOF_TAPS);
        float a = i * 2.3999632;
        float dist = t * R;
        float2 u = uv + float2(cos(a), sin(a)) * dist * dofStep.xy;
        float4 s = tex2Dlod(src0, float4(u, 0, 0));
        float tc = s.a * 32;
        float eff = aoViewZ(u) > zc ? min(tc, cc) : tc;
        float w = saturate(eff - dist + 1);
        sum += s.rgb * w;
        wsum += w;
        csum += eff * w;
    }
    return float4(sum / wsum, csum / wsum / 32);
}

float4 psDofComp(float2 uv : TEXCOORD0) : COLOR {
    float3 sharp = tex2D(src0, uv).rgb;
    float4 b = tex2D(src1, uv);
    float coc = max(dofCoc(aoViewZ(uv), dofPlanes()), b.a * 32);
    float f = smoothstep(0.25, 1.0, coc);
    if (dofBlurP.w > 0.5) return float4(f, f, f, 1);
    return float4(lerp(sharp, b.rgb, f), 1);
}

// ---- Sun flare + glare -------------------------------------------------------------------------
// Visibility: a 1x1 target eases towards the share of sky taps in a small disc around the sun (so
// the flare fades rather than pops behind edges); stored as vis + 1 so 0 = no history yet. The flare
// itself is procedural (core, halo, starburst, anamorphic streak, ghosts along the sun -> centre
// axis, a halo ring, veiling glare), added on. Glare: bright pass -> two wide blurs -> added on.
float4 flareSun : register(c26);   // xy: sun uv, z: aspect (w/h), w: strength (intensity * on-screen fade)
float4 flareCol : register(c27);   // rgb: flare colour, w: ghost strength
float4 flareParm : register(c28);  // x: halo/core strength, y: streak strength, z: veiling glare, w: visibility disc radius (uv)
float4 flareMisc : register(c29);  // x: visibility ease factor this frame, y: 1 = no depth (always visible)
float4 glareParm : register(c30);  // x: threshold, y: intensity, zw: blur tap step (uv)
sampler2D flareVisTex : register(s4);

float4 psFlareVis(float2 uv : TEXCOORD0) : COLOR {
    float sum = 0;
    [unroll] for (int i = 0; i < 16; i++) {
        float t = sqrt((i + 0.5) / 16);
        float a = i * 2.3999632;
        float2 u = flareSun.xy + float2(cos(a) / flareSun.z, sin(a)) * t * flareParm.w;
        sum += (flareMisc.y > 0.5 || aoViewZ(saturate(u)) >= 1e6) ? 1 : 0;
    }
    float cur = sum / 16 + 1;
    float prev = tex2Dlod(src1, float4(0.5, 0.5, 0, 0)).r;
    float f = prev > 0.5 ? lerp(prev, cur, flareMisc.x) : cur;
    return float4(f, f, f, 1);
}

static const float kGhostT[6] = {0.45, 0.75, 1.15, 1.4, 1.7, 2.2};
static const float kGhostR[6] = {0.035, 0.06, 0.02, 0.09, 0.045, 0.14};
static const float3 kGhostC[6] = {float3(1.0, 0.75, 0.4), float3(0.45, 0.85, 0.6), float3(0.9, 0.5, 1.0),
                                  float3(0.4, 0.6, 1.0), float3(1.0, 0.9, 0.55), float3(0.5, 0.7, 1.0)};

float3 flareColor(float2 uv, float vis) {
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
    [unroll] for (int i = 0; i < 6; i++) {
        float2 gp = flareSun.xy + axis * kGhostT[i];
        float gd = length((uv - gp) * asp);
        float disc = saturate((kGhostR[i] - gd) / (kGhostR[i] * 0.35));
        o += kGhostC[i] * c * disc * (0.06 + 0.06 * (gd / kGhostR[i])) * flareCol.w;
    }
    float2 hc = (uv - 0.5) * asp;
    float hr = length(hc);
    float2 sd = (flareSun.xy - 0.5) * asp;
    float side = saturate(dot(hc / max(hr, 1e-4), sd / max(length(sd), 1e-4)));
    float ring = exp(-pow((hr - 0.42) / 0.02, 2));
    o += float3(0.6, 0.8, 1.0) * c * ring * side * side * 0.06 * flareCol.w;
    o += c * flareParm.z * (0.15 + 0.85 * exp(-r / 0.35)) * 0.25;
    return o * flareSun.w * vis;
}

float4 psFlare(float2 uv : TEXCOORD0) : COLOR {
    float vis = saturate(tex2Dlod(flareVisTex, float4(0.5, 0.5, 0, 0)).r - 1);
    return float4(flareColor(uv, vis), 1);
}

float4 psGlareDown(float2 uv : TEXCOORD0) : COLOR {
    float3 c = tex2Dlod(src0, float4(uv, 0, 0)).rgb;
    float l = max(c.r, max(c.g, c.b));
    float k = saturate((l - glareParm.x) / max(1 - glareParm.x, 0.05));
    return float4(c * k * k, 1);
}

float4 psGlareBlur(float2 uv : TEXCOORD0) : COLOR {
    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int i = -6; i <= 6; i++) {
        float w = exp(-i * i / 18.0);
        sum += tex2Dlod(src0, float4(uv + glareParm.zw * i, 0, 0)).rgb * w;
        wsum += w;
    }
    return float4(sum / wsum, 1);
}

float4 psGlareComp(float2 uv : TEXCOORD0) : COLOR {
    return float4(tex2D(src0, uv).rgb * glareParm.y, 1);
}

// ---- Screen-space weather (rain / snow) --------------------------------------------------------
// Four layers of drops on cylinders around the camera (a tenth, a quarter, half and all of the
// distance). Columns are angles around the camera (a whole number of them, so no seam) and rows are
// world height on the cylinder, so turning the camera keeps the drops still in the world and nearer
// layers fall faster on screen. A layer only shows where the scene is farther than it. Drops fall
// along the wind (sheared cells); the pattern repeats every 64 rows so the scroll can wrap.
// The view ray and depth decoding share the fog's (c8..c10) and the AO's (c17) constants.
float4 wxParm : register(c31);   // x: 1 rain / 2 snow, y: drop density (0..1), z: time (s, wrapped), w: farthest layer distance
float4 wxWind : register(c32);   // xy: wind (world units/s), z: fall speed (units/s), w: pixel size at distance 1 (world units)
float4 wxColor : register(c33);  // rgb: colour, w: opacity

float wxHash(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }

float wxLayer(float3 dir, float hl, int li, float sceneDist) {
    bool snow = wxParm.x > 1.5;
    float d = wxParm.w * (li == 0 ? 0.1 : li == 1 ? 0.25 : li == 2 ? 0.5 : 1.0);
    float N = snow ? 46 + 14 * li : 96 + 22 * li;  // columns around the camera
    float aspect = snow ? 1 : 3;                   // cell height / width
    float cellW = d * 6.2831853 / N, cellH = cellW * aspect;
    float X = (atan2(dir.y, dir.x) / 6.2831853 + 0.5) * N;
    float Y = d * dir.z / hl / cellH;
    float2 tng = float2(-dir.y, dir.x) / hl;
    float fall = max(wxWind.z, 1);
    float slant = dot(wxWind.xy, tng) / fall * (cellH / cellW);
    float S = X + slant * Y;
    float Yd = Y + frac(wxParm.z * fall / (64 * cellH)) * 64;
    float pxc = wxWind.w * N / 6.2831853;  // one pixel, in cells (horizontally)
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

float4 psWeather(float2 uv : TEXCOORD0) : COLOR {
    float3 scene = tex2D(src0, uv).rgb;
    float3 ray = fogRay0.xyz + uv.x * fogRayX.xyz + uv.y * fogRayY.xyz;
    float rl = length(ray);
    float3 dir = ray / rl;
    float sceneDist = aoViewZ(uv) * rl;
    float hl = length(dir.xy);
    float pole = saturate((hl - 0.3) / 0.25);  // fades out looking straight up or down
    float a = 0;
    if (pole > 0) {
        [unroll] for (int li = 0; li < 4; li++) a = 1 - (1 - a) * (1 - wxLayer(dir, hl, li, sceneDist));
    }
    a *= pole * wxColor.w;
    return float4(lerp(scene, wxColor.rgb, a), 1);
}

// ---- Local light shafts (point lights and spotlights) -----------------------------------------
// Per light, at half res: the light scattered towards the camera along each view ray inside the
// light's sphere, up to the scene depth (the baker's falloff (1 - d^2/R^2)^2, times the spot cone),
// marched in 24 jittered steps, plus a bright core near the bulb integrated exactly
// (k^2 / (d^2 + k^2) along the ray). Alpha = 1 where the scene lies at or behind the light (free
// air), 0 for geometry in front of it. Then a radial pass towards the light's screen position
// averages that mask: geometry between the pixel and the light on screen casts streaks of shadow
// through the glow. Each light is added into a half-res accumulator, which is added onto the frame.
// View rays: c8..c10, eye: c11 (xyz), depth decoding: c17.
float4 lPos : register(c34);     // xyz: light position, w: radius
float4 lCol : register(c35);     // rgb: colour * intensity * scattering density
float4 lSpot : register(c36);    // xyz: spot direction (unit), w: cos of the outer angle (< -1.5: point light)
float4 lMisc : register(c37);    // x: cos of the inner angle, y: shadow strength, z: jitter offset (per frame), w: 1 = shadow pass on
float4 lScreen : register(c38);  // xy: light uv, z: how far towards the light the shadow pass looks (fraction), w: unused

float lCone(float3 v, float d) {
    return lSpot.w < -1.5 ? 1 : smoothstep(lSpot.w, lMisc.x, dot(v, lSpot.xyz) / max(d, 1e-3));
}

float3 lightScatter(float3 eye, float3 dir, float L, float2 pix) {
    float3 oc = eye - lPos.xyz;
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
    // the core: closest approach tm, miss distance hh
    float tm = -b;
    float3 vc = oc + dir * clamp(tm, t0, t1);
    float hh2 = max(dot(oc, oc) - b * b, 0);
    float k2 = 0.02 * R2, s = sqrt(hh2 + k2);
    float core = k2 / s * (atan((t1 - tm) / s) - atan((t0 - tm) / s));
    float fc = saturate(1 - dot(vc, vc) / R2);
    sum += 2 * core * fc * fc * lCone(vc, length(vc));
    return lCol.rgb * sum;
}

float4 psLightScatter(float2 uv : TEXCOORD0) : COLOR {
    float3 ray = fogRay0.xyz + uv.x * fogRayX.xyz + uv.y * fogRayY.xyz;
    float rl = length(ray);
    float3 dir = ray / rl;
    float L = aoViewZ(uv) * rl;
    float3 S = lightScatter(fogEye.xyz, dir, L, uv * 1024);
    float free = L > length(lPos.xyz - fogEye.xyz) - 0.25 * lPos.w ? 1 : 0;
    return float4(S, free);
}

#define LTAPS 24
float4 psLightShaft(float2 uv : TEXCOORD0) : COLOR {
    float4 c = tex2Dlod(src0, float4(uv, 0, 0));
    float vis = 1;
    if (lMisc.w > 0.5) {
        float2 delta = (lScreen.xy - uv) * (lScreen.z / LTAPS);
        float sum = 0, wsum = 0, w = 1;
        float2 p = uv;
        [unroll] for (int i = 0; i < LTAPS; i++) {
            p += delta;
            sum += tex2Dlod(src0, float4(saturate(p), 0, 0)).a * w;
            wsum += w;
            w *= 0.96;
        }
        vis = lerp(1, sum / wsum, lMisc.y);
    }
    return float4(c.rgb * vis, 1);
}

float4 psLightComp(float2 uv : TEXCOORD0) : COLOR { return float4(tex2D(src0, uv).rgb, 1); }
