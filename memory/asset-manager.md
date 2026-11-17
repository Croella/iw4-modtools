---
name: asset-manager
description: "Asset Manager window (F4 / View / toolbar 'Assets'): zone_source index + analysis + weapon editor + import/rename/delete; core in src/assetmanager, UI in src/assetpanel.cpp"
metadata:
  type: project
---

Added 2026-11-16 (user: "Make an asset manager for guns, xmodels, xanims, etc").
Core (no UI, tested by testAssetManager): src/assetmanager.{h,cpp} -- Index::scan (weapons, xmodels
+ _lodN, xanims, images in images/ AND xmodels/ keyed by lowercase file name, fx/**.efx by path,
skies, scripts, Other), analyse() on a worker thread (model stats/materials via findTexture = the
compiler's texture search, anim frames/notetracks/best-matching skeleton, weapon refs resolved with
zsrc::resolve, usedBy map, Issues), WeaponFile (ordered kv, lossless round trip), importFile (xmodel
brings LOD siblings + textures beside it), renameAsset (keeps _LOD suffix case, rewrites weapon refs),
duplicateWeapon, newWeapon (AK-74u non-zero stats template), deleting goes to the Recycle Bin.
weapondef::refKind/animKey and zsrc:: helpers are exported for it.
UI: tree with problem colouring, Problems list, per-kind details (model thumbnail, anim "Use in
weapon", image preview), weapon editor tabs (Models & anims with zone_source pickers + resolve
status, Stats, All keys, Compiler notes). Missing model textures = one issue per model, Info for
weapon gun/world models (compiler drops untextured parts). CLI: iw4radiant <map> --assets <name>.

**Mod Tools (2026-11-16, user: "I want radiant to be a secondary tool. Make a iw4-modtools base"):**
iw4modtools.exe (src/modtools/) is now the main program: Maps panel (maps/<map>/ folders, info,
New Map creates folder + .map + .gsc + zone_source/), the Asset Manager docked for the selected map,
Console; launches iw4radiant.exe as a separate process (double-click a map / Open in Radiant), tracks
it, rescans when it closes. The panel is class AssetPanel (src/assetpanel.h) + AssetHost interface;
App (Radiant) and ModTools both implement AssetHost (Radiant adds place/use-on-selection/entity
renames; ModTools renames models in the .map text). Shared UI lib iw4r_ui (renderer, assets,
assetpanel, modelpreview::load, uistyle.h). Radiant has File > Open Mod Tools. Mod Tools layout in
iw4modtools.ini, last map in build/Release/iw4modtools_last.txt. CLI: iw4modtools [map] --asset <n>.

Related: [[weapons]], [[zone-source]], [[map-folder-layout]].
