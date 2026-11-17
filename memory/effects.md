---
name: effects
description: "FX compiler: .efx (iwfx 1-3) -> x64 FxEffectDef, conversion verified vs stock; BO1 materials/images auto-imported; fx_origin entity -> generated script; awaiting in-game test"
metadata:
  node_type: memory
  type: project
  originSessionId: c635133b-e9c7-4023-9bad-d8499390d538
  modified: 2026-11-15T23:30:45.210Z
---

User asked "Add fx compiling" (2026-11-14); chose .efx authoring + editor fx entity.

**Format/RE (all verified):** x64 FxEffectDef 40 / FxElemDef 288 (offsets = iw4_structs.h widened;
loader MP 0x118f30 -> 0x1191c0, visuals 0x119460/0x1196a0, extended 0x1195d0, trail 0x119e20).
Runtime: velocity 0x160940, position 0x15c410, vis 0x13a980/0x13bf20. Flags = Xbox PDB header
FX_ELEM_* (C:\Users\Lurkz\Desktop\iw4mp pdb\2-iw4mp.exe.h ~line 16048).
**Ground truth:** BO1 raw/fx/muzzleflashes/saw_flshview_c.efx == MW2 common.ff compiled
muzzleflashes/saw_flshview_c (every field matched; unit test testEffects asserts it). Rules in
src/zone/efx.h header comment (vel/(1000N), size/2, BGRA, deg->rad, gravity/100, modColorByAlpha ->
rgb*a + alpha 255, totalSize formula exact, msecLoopingLife = max interval*(count-1)).
UNKNOWN/guessed: atlas loopCount (max(1,efx)), light scale 1.5 default, sample-count choice (ours
adaptive), absVel/useBackCompatVel ignored, playOnTouch/Death/Run elements SKIPPED, BO1 line->tail,
rotatedSprite->billboard, spark types unsupported.

**Code:** src/zone/efx.{h,cpp} (parser+compiler), zonebuild loadZoneSourceEffects/writeFx/
writeFxMaterial/fxPlacementScript/injectIntoMain, src/zone/bo1fx.{h,cpp}, iwi.cpp v13. zonesim now
loads fx (30), localize (27), stringtable (37) -> simulatePrefix gets 5229 assets into common.ff.
CLI: `iw4r_zone fxprobe <ff> [name] [--samples|--materials]`, `iw4r_zone efxcheck <dir> [n]`
(all 4027 BO1 efx parse, 3898 compile, whole corpus as one zone passes the loader sim).
Materials: zone_source (images/<name>.png + fx/materials/<name>.txt) > stock by name (atlas from
cached common.ff scan %LOCALAPPDATA%\iw4-radiant\fxmaterials_common.txt) > BO1 raw/materials +
iwd DXT images (bo1::defaultRoot = sibling "Call of Duty Black Ops" or IW4R_BO1); BO1 distortion
-> stock gfx_distortion_heat_far; unresolved -> element dropped. Effect material template =
common.ff effect materials (sortKey 48, stateFlags 0x13, camRegion 2, techsets effect_[zfeather_]
add|blend[_nofog] / particle_cloud by name; stateBits blend 19289165, add 19218922, zfeather add
09218922, [1] 88128812 e49e492c; consts featherParms 0x4D7EA234, colorTint 0xB60C3B3A).
fx_origin (keys fx, angles default 270 0 0, delay) -> maps/[mp/]<map>_fx.gsc (spawnfx+triggerfx or
playfx loop), call injected first in map script main(). Sample: zm_test has 9 fx_origins with 4
BO1 effects copied into maps/zone_source/fx/env/, plus 2 of the hand-written commented example
fx/env/nature/fx_fireflies.efx (BO1 material gfx_fxt_light_flare2_eo15; efx supports // comments). NOT tested in-game; MP common_mp techsets assumed.
Related: [[weapons]], [[zone-source]], [[world-materials]].

**Editor live preview (2026-11-15):** src/fxpreview.{h,cpp} CPU sim of zone_source/fx effects on fx_origins
(sprite types only; vel0 in effect axes, vel1 world, gravity % of 800, looping ignores spawnOneShot,
one-shot effects replay every delay=2s like the generated script); materials via
zone::FxPreviewMaterials (zonebuild.h, same lookup order as the compiler) -> Renderer::setTextureImage;
Renderer::setSprites draws premultiplied add/blend sprites (RGB-only write: alpha = sky marker).
Auto-starts after F9 bake and after compile; View > Effects Preview / Reload Effects. testFxPreview.
Firefly example size raised to 28 / spawn 175ms: BO1 flare2_eo15 texture is a tiny core (avg 12/255).

**Infinite loops (fixed 2026-11-15, user saw "fx play once and disappear" in-game):** efx spawnLooping
count 0 = forever must compile to elem spawn.looping.count 0x7FFFFFFF AND effect msecLoopingLife
0x7FFFFFFF (stock fire/firelp_med_pm, propane_small_fire; raw scan `iw4r_zone fxprobe <ff> --loops`).
We wrote 0/0 before. Regression check in testEffects (fireflies).

**Dynamic fx (2026-11-15):** fx_origin keys targetname/duration/once/start_off; trigger_radius with
target=<name> starts the group on entry (script_noteworthy "stop" stops); scripts use level
notify("<name>") / ("<name>_stop"). zonebuild fxPlacements() + fxPlacementScript dynamic()/watch()
(spawnstruct waitstop helpers, istouching once-per-entry). Brush triggers work too (see
[[brush-triggers]]). Preview: camera acts as player in trigger_radius / brush triggers; Lighting >
Effect Groups start/stop. zm_test: zm_fire_trap at -450 450 (trigger_multiple brush).
Generated GSC NOT run in-game yet (only brace/text checks in testFxPreview).
