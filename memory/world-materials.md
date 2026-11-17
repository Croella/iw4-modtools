---
name: world-materials
description: "Why compiled maps showed the unlit default material, and how world materials are now embedded (matdef library)"
metadata:
  node_type: memory
  type: project
  originSessionId: c635133b-e9c7-4023-9bad-d8499390d538
  modified: 2026-11-11T22:38:20.269Z
---

**Problem (found 2026-11-11 with `iw4r_zone liveworld --sp`):** compiled maps referenced world
materials by name (",wc/foo"). World materials only ship inside the stock MAP zones that use them,
so in-game they resolved to the DEFAULT material (techset "2d", technique trivial_vertcol_simple2d,
16x16 "default" image): unlit, so lightmaps/sun never showed. Lighting data itself was correct.

**Fix:** src/zone/matdefs.{h,cpp} scans stock zones for fully-inline "wc/" materials (struct 0x78,
name, techset skipped, texture table + inline image headers, constants, state bits) into a library
cached at %LOCALAPPDATA%\iw4-radiant\materialdefs.bin (2272 materials from all SP+MP zones, ~77 s;
editor AssetDb builds it during its zone scan, CLI `iw4r_zone matdefs <zone dir>`; `matdefs show X`).
Compiler (writeWorldMaterial) writes each used material in full: techset as BY-NAME stub
",wc_l_sm_b0c0[n0][s0]" (common.ff has these; c0=colorMap n0=normalMap s0=specularMap, learned from
the sets' PS samplers), images inline once (stock header, category 3, loadDef resourceSize 0 ->
stream images/<name>.iwi, even "~..._spc-rgb&..." names exist in IWDs) then back-referenced.
Unknown materials stay by-name + a compile warning. Test: testMaterialEmbed. NOT yet verified in-game.

**Key RE fact:** a material's techset back-ref in a stock zone points at the zone's ASSET ARRAY
header slot: index = ((ref-1)&0x0FFFFFFF - arrayStart - 8)/16, arrayStart = align4(8*stringCount +
inline string bytes) in VIRTUAL. (Used to confirm; names of top-level techsets weren't reliably
recoverable by pattern scan, hence the by-name approach.)
Diagnose in-game with `iw4r_zone liveworld [--sp]` (surfaces -> material, techset, techniques,
images, laf, lightmaps, sun). See [[lighting]].

**Effect materials (2026-11-11):** matdef::shadingFor classifies by constants: Lit (colorTint/
envMapParms/detailScale/uvAnimParms only), FalloffAdd (falloff* consts: stock
wc_unlit_falloff_add_lin[_ua], 17 mats incl. wc/atmos_particles_dust = scrolling additive dust
sheets), Unsupported (distortion...). Stock falloff techset's PS/decl/literal are zone back-refs in
EVERY SP zone (shared), so we use OUR OWN shaders: tools/shaders/falloff_add_ua.hlsl compiled by
tools/shaders/build.py (d3dcompiler_47) into src/zone/effectshaders.h, registers mirroring the stock
VS (c0 viewProj, c4 world, c13 falloffParms, c14/15 begin/end, c20 uvAnimParms, c21 fogConsts,
c22 gameTime, s0 colorMap). writeFalloffTechset: inline techset "iw4r_falloff_add_ua" (slot 4, 9
args copied from stock minus pixel literal; code-const u = u16 index | u8 firstRow<<16 | rows<<24),
decl routing 00 00/01 02/02 05/03 01. Missing uvAnimParms constant added as 0. Unlit surfaces
(lightmap 31, no sun), excluded from the bake (light::Settings::unlitMaterials). Editor preview:
additive, depth-test-only, uv rotate+scroll by time, view-angle falloff. zonesim gained top-level
techset/material/image + simulatePrefix/resolve (`iw4r_zone refprobe`). NOT yet verified in-game.
**Crash fix (2026-11-11):** embedded techsets with ONLY slot 4 crashed iw4sp at +0x496d0 (material
sort comparator: if one material has techniques[9] it reads the other's techniques[9]->passArray[0]
unchecked; lit common techsets have slot 9). Stock effect/sky sets point slots 9..36 at one shared
technique -> writeEffectTechsetHeader back-references slots 9..36 to our slot-4 technique (VIRTUAL
offset = align4(posIn(VIRTUAL) + name + NUL), asserted). Rule: embedded techsets must fill 9..36.
**Surface order (2026-11-11):** stock GfxWorld surfaces are SORTED by material sortKey (MaterialInfo
byte 9): trainer litOpaque [0,2957) = keys <=2 (sky=2 last), litTrans [2957,4126) = 6..29 (decals,
atmos dust 29), shadowCaster 34, emissive empty. We had everything in litOpaque -> the additive dust
loaded fine (liveworld showed it) but never drew. Fix: gfx::sortSurfaces by key (embedded = stock
key, sky = 2, by-name = 1), opaque = key <= 5, dpvs litOpaque/litTrans ranges from that.
`IW4R_DUMP_DPVS=1 iw4r_zone import <stock.ff>` prints a stock map's ranges + material order.
**Dust visibility (2026-11-11):** (1) our embedded shader structs had loadForRenderer (+26) = 1;
stock is 0 -> fixed to 0 (likely why the dust never drew). (2) atmos dust stateBits cull BACK
(ONE/ONE additive); brushes face outward, so a dust VOLUME is invisible from inside. Added per-brush
"Two-sided" toggle (brush attr line `iw4r_twosided;`, Brush::twoSided/setTwoSided, mirroredFace):
compiler emits mirrored copies, editor previews them; View > Back-Face Culling (F6, `--cull`).
(3) Effect materials are now NONSOLID (buildClipMapData nonSolid set = the unlit/effect set);
before, dust brushes were invisible solid walls. Our VS is instruction-identical to stock.
