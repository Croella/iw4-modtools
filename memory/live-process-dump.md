---
name: live-process-dump
description: "Editor feature + RE'd addresses for dumping the running game's map models/materials via ReadProcessMemory"
metadata:
  node_type: memory
  type: project
  originSessionId: 5594d009-4322-40bf-84d3-5c0c822a3f25
  modified: 2026-11-11T20:48:00.626Z
---

iw4-radiant can dump the loaded map's placed static models + materials straight out of the
running game and import them as reference geometry. Added 2026-11-11. External ReadProcessMemory
only — no injection, nothing written to the game. (Decided over an injected-DLL dumper.)

**Code:** `src/live/process.{h,cpp}` (ProcessReader: attach to iw4mp.exe VM-read, read structs/
arrays/strings; runtime addr = moduleBase + IDA RVA) + `src/live/liveimport.{h,cpp}`
(`live::importFromProcess` → fills a Document's RefMesh). UI: File > "Import Models from Running
Game" (app.cpp `importLiveModels`). Headless: `iw4r_zone livedump [out.map]`.

**RE'd globals** (image RVAs; runtime = moduleBase + RVA). THESE MOVE EVERY GAME UPDATE — re-decode
the pool allocator from the new binary each time (find the "Exceeded limit of %d '%s' assets" string,
scan .text for the `lea rcx,[rip->string]`, disassemble ~0x100 before it: the allocator indexes
`[type*8 + DB_XAssetPool]`, `[type*4 + g_poolSize]`, `[type*8 + g_assetNames]`). A capstone script on
the raw PE does this fast; no full IDA analysis needed. ASSET_TYPE_GFXWORLD = 0x15.

- Build **2.0.14** (`C:\Users\Lurkz\Desktop\iw4mp_2014.exe`, a 156MB unpacked dump; live game is the
  packed 6.25MB Steam iw4mp.exe): DB_XAssetPool=**0x407D60**, g_poolSize=**0x407890**,
  g_assetNames=**0x407280** (alloc fn table 0x407AA0; allocator at RVA 0x12CEF0). These are in
  src/live/liveimport.cpp. NOT yet confirmed against the live process.
- **iw4sp.exe 2.0.14** (unpacked dump `C:\Users\Lurkz\Desktop\iw4_2014_sp.exe`, 76MB): DB_XAssetPool=**0x3BFE00**,
  g_poolSize=**0x3BF930**, g_assetNames=**0x3BF360** (allocator RVA 0xEEE30). Asset-type enum identical to MP.
  VALIDATED live 2026-11-11 (maps/trainer: 6772 instances). liveimport.cpp `kGames[]` holds both games;
  SP's XModel pool has an 8-byte free-list head (entries at pool+8, stride 408); loadLiveModels
  auto-detects head 0 vs 8 by counting readable names. Entity models validated on SP 2026-11-11.
  exeName=nullptr auto-picks whichever is running (MP first); CLI `--mp`/`--sp` forces one.
- Build 2013 (iw4mp_2013.exe.i64, now OUTDATED): DB_XAssetPool=0x421D60, g_poolSize=0x421FD0,
  g_assetNames=0x421A40, XAssetEntry pool=0x1521660 (stride 24). Found via allocator sub_12CDE0.
The x64 build uses per-type free-list allocator functions, so DB_XAssetPool[type] is NOT a simple
array index for all types; for the singleton GfxWorld, DB_XAssetPool[0x15] + name check works.
Diagnose with `iw4r_zone liveprobe` (prints base + the asset-type-name table + GfxWorld pool/count;
names reading back as "xmodel"/"material"/... proves the addressing).

**Entity models (2026-11-11):** File > "Load Entity Models from Running Game" (App::loadEntityModels) reads
the XModel named by each point entity's `model` key from the game's XModel pool (type 4, 1536 entries,
408-byte structs inline in the pool like GfxWorld; pointer-array layout tried as fallback; free-list
entries skipped because their first qword points back into the pool) into a `live::LiveModelLibrary`
(model-space top-LOD meshes, key = lower-case name). Renderer::rebuild transforms each by the entity's
origin/angles (CoD AnglesToAxis: x fwd, y left, z up)/modelscale, so models follow entities while
editing; "*N" brush-submodel names skipped. Only models the game currently has loaded resolve.
CLI: `iw4r_zone livemodels <name...>`. static_asserts pin XModel=408, materialHandles@88, lodInfo@96x56.
NOT yet validated in-game (game wasn't running when built).

**Traversal:** DB_XAssetPool[GFXWORLD] -> GfxWorld (name "maps/mp/..d3dbsp") -> dpvs
(GfxWorldDpvsStatic) .smodelCount/.smodelDrawInsts -> GfxStaticModelDrawInst{GfxPackedPlacement
placement(origin,axis3x3,scale), XModel* model} -> XModel.lodInfo[0] -> XModelSurfs.surfs ->
XSurface{verts0(GfxPackedVertex), triIndices}; material = XModel.materialHandles[lod.surfIndex+j].
All layouts from src/game/iw4_structs.h (via [[x64-struct-layouts]]). Vertex decode reuses the
GfxWorld importer's PackedUnitVec normal unpack; model UVs are 2x fp16 in GfxPackedVertex.texCoord.

**Why:** lets the user pull a map's real models/materials into the editor without owning the
source .map or reversing every asset's zone layout.

**Status: VALIDATED in-game 2026-11-11** on mp_favela (2.0.14). `liveprobe` read the whole asset-
type-name table correctly and found GfxWorld at the gfx_map pool; `livedump` + the editor menu
imported the map's models. DB_XAssetPool[0x15] points directly at the GfxWorld struct (inline
layout; name at offset 0). Module base was 0x7FF76CFC0000 (no ASLR on that run).
- Import MERGES into the current Document's refMesh (appendMesh) instead of resetting it, so dump
  ONTO an already-open .ff map (import the .ff first, then dump). A repeat dump REPLACES the previous
  dump rather than stacking: RefMesh tracks hasLiveDump + liveVertStart/liveIndexStart/liveBatchStart
  (where the dumped geometry begins); importFromProcess truncates back to those before re-appending,
  keeping non-dump geometry (the .ff map, brushes), then recomputes bounds.
- Still worth a look if models seem off: placement axis convention (orientation/winding) and the
  fp16 UV decode. Positions are exact regardless.
See [[zone-linker]] (RefMesh import path), [[reference-iw4-symbols]] (IDA tooling), [[client-integration]].
