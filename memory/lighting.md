---
name: lighting
description: "IW4 lightmap/sun encoding (RE'd from stock shaders) + the editor/compiler light baker (src/light/)"
metadata:
  node_type: memory
  type: project
  originSessionId: c635133b-e9c7-4023-9bad-d8499390d538
  modified: 2026-11-11T21:55:28.842Z
---

**IW4 world lighting formula** (RE'd 2026-11-11 from stock lit_lmap ps_3_0 shaders in trainer.ff,
disassembled with d3dcompiler_47 D3DDisassemble; v1.zw = lmapCoord):
`out = albedo^2 * ( (T.rgb + B.rgb * dirTerm)^2 + primary.r * sunColor * saturate(N . sunDir) )`
- **secondary** (A8R8G8B8, W x 2H): two vertically stacked halves at the SAME uv: T at (u, v*0.5),
  B at (u, v*0.5+0.5). Alphas give a tangent-space dir d = (aT*4.08-2.08, aB*4.0645-2.0645);
  flat: dirTerm = 1/sqrt(1+|d|^2); normal-mapped: like a normal map (x along tangent, y binormal).
  Values are sqrt(linear) -> up to 4x overbright. Byte 130 alpha = no direction.
- **primary** (L8, 2W x 2H, i.e. 2x density) = direct-SUN VISIBILITY mask. The sun itself is the
  real-time sun primary light (ComWorld light 1, lightPosition c17 / lightDiffuse c18), so baked
  light must EXCLUDE direct sun. Surfaces lit by it: laf.primaryLightIndex = 1.
- Light grid GfxLightGridColors rgb[56][3] byte scale is UNVERIFIED (we write 128 = 1.0 linear,
  replicated over all 56); entries' primaryLightIndex = 1 where the sun reaches the cell.

**Baker** `src/light/lightbake.{h,cpp}` (light::bake): worldspawn keys sundirection/suncolor/
sunlight/diffusefraction/sundiffusecolor/ambient/_color; `light` ents (origin, _color, intensity,
radius|light). Float-tri BVH, threaded; passes: sun mask (2x) -> direct -> sky dome + N bounces
(cos-weighted). Faces projected on dominant axis, shelf-packed pow2 atlas (luxel grows to fit
maxAtlas). sceneHash() = staleness (doc.revision also bumps on selection). Light grid sampled on a
coarse lattice (<=40k samples).
**Compiler**: CompileOptions{bakeLighting, light, prebaked, progress} on compileMapToFastfile;
tests use kNoBake (uniform placeholder) except testLightingBake. CLI compile bakes by default.
**Editor**: Lighting menu + F9 (background bake), preview shader uses the same formula (sky faces
drawn as sky tint), compile reuses an up-to-date bake or bakes synchronously. `--bake` cmd flag for
screenshots. Linking a test exe while the user's editor is open:
`MSYS_NO_PATHCONV=1 cmake --build build --config Release --target iw4radiant -- /p:TargetName=x`.

**Sky (2026-11-11):** no sky material/techset/shader exists in any always-loaded zone (common,
code_*), each stock map carries its own, so compiled maps EMBED one. src/zone/skysource.{h,cpp}
copies from the installed SP trainer.ff (cached %LOCALAPPDATA%/iw4-radiant/sky_source.bin):
wc_sky techset slot 4 "sky" (1 pass; sky.hlsl vs 400 B = position only w=0, ps 260 B = cube texld
s4 squared; 3 args: code consts + colorMap sampler hash 0xa0ab1041), material wc/sky_sp_trainer
struct + texdef + 2 stateBits, image header (cube, category 3, loadDef resourceSize 0 -> streams
images/sky_*_ft.iwi from iw_02.iwd). Stock pass's vertex decl + techniques 9-36/44 are zone
back-refs -> decl rebuilt ("pp": routing pairs at +0x10, (src,dst) 00 00), other slots null.
GfxWorld: sky faces -> surfaces of embedded "wc/iw4r_sky" (lightmap 31), skies[0] {surf list,
skyImage inline+slot, sampler = texdef[6]}; material texdef back-refs that image slot.
worldspawn "sky" "sky_mp_boneyard" picks the cubemap (else the sky face material, default
sky_sp_trainer). Loader-sim verified (testSkyCompile); NOT yet verified in-game.
Test map: maps/basic.map (floor ch_asphalt01, box kh_concrete_01, sky shell, light, info_player_start).

**GPU bake (2026-11-16):** src/light/gpubake.{h,cpp}: D3D11 cs_5_0 kernels csSun/csLights/csGather on the same BVH
(Scene tris/nodes uploaded as structured buffers), chunks sized from the measured ray rate (~120 ms, TDR-safe), cancel
between chunks. light::Settings::gpu (default on; editor Lighting menu checkbox; CLI `--cpubake`), Bake::device / gpuNote;
falls back to the CPU passes on any failure. Light grid (sampleGrid) still CPU. `iw4r_zone bakebench <map> [luxel]`:
zm_test luxel 4 -> 1024^2 atlas: CPU 7.8 s vs GTX 1660 SUPER 0.8 s, sun mask identical, light total +0.00%.
**Engine-native lighting upgrades (2026-11-16, user: "Do all of them in order"; all loader-sim + unit tested, NOT in-game):**
1. Directional lightmaps: stock normal-mapped lit_lmap PS = (T/|(n,1)| + B*sat(dot(norm(d,1),norm(n,1))))^2, d from the
   alphas, tangent frame x = vertex tangent, y = binormalSign*cross(N,T) (stock world VS). Bake::lightDir (lum-weighted
   dir sum + weight per luxel, CPU+GPU); gfxworld encodeDirectional splits sqrt(E) into T/B by directionality*0.9.
2. Light grid: model PS samples a 3D texture at base + n/max|n|*scale, returns (2*texel)^2 -> 56 colours = surface voxels
   of a 4x4x4 block, z slowest then y then x, inner 2x2x2 skipped (engine corner avg uses 0 3 12 15 40 43 52 55); bytes
   RGB, = sqrt(E)*127.5 (we used to write E*128: wrong). Checked on mp_afghan (layer 0 = sand bounce below, 3 = sky).
   light::gridDirections(); sampleGrid 96 rays + the lights; GridSample::primary.
3. Primary spotlights: light "primary" "1" (spot only, <= 32). Stock ComPrimaryLight spot: type 2, canUseShadowMap 1,
   exponent 1, dir = -spotDir, cosExpanded = cosOuter, rotationLimit 1, defName "light_point_linear" (we always use it;
   UNVERIFIED that the def is loaded globally). Face -> FaceMap::primary (strongest reach); spot faces: sunVis = spot
   shadow mask, sun baked in, spot excluded (Bake::realtime = excluded part). Surfaces split by primary. Shadow geom =
   surfaces touching the region box; light region = 1 hull, kdop axes x y z x+y x-y x+z x-z y+z y-z (unnormalized,
   origin-relative; RE'd from stock hulls), no extra axes. Grid entry primaryLightIndex = 2+spot where it reaches.
   Stock spot PS: falloff tex * pow(sat(dot(L,spotDir)*f.x+f.y), f.z) * primaryLightmap.r * N.L * lightDiffuse.
4. Reflection probes: stock = 64x64 cube A8R8G8B8, levels 0 (full mips), flags 0x10000, face-major (mips per face),
   texel = sqrt(radiance) (PS: texldl lod 6-8*gloss, squared). light::bakeReflectionProbe ray-traces from the bake
   (albedo 0.4, sky + sun glow). reflection_probe entities (<= 64), else one 48 up from the first spawn; surfaces split
   by nearest probe; cells list all.
5. Lightmap pages: Settings::maxPages (4) of maxAtlas^2; Bake::pageY/pageH stack pages in one virtual atlas (editor
   unchanged); lmapCoordPage for vertices; surfaces split by page; laf.lightmapIndex = page. Stock: 1-2 pages <= 1024.
Debug: IW4R_DUMP_LIGHTGRID=1 iw4r_zone import <stock.ff> prints grid averages, primary lights/regions, lightmaps,
probes (IW4R_DUMP_ZONE / IW4R_DUMP_PROBE save raw data).
**Not done yet:** in-game verification of all of the above.
See [[zone-linker]], [[compile-and-run]].
