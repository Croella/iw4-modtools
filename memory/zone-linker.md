---
name: zone-linker
description: "iw4-radiant zone compiler status - the reversed loader stream model, what the linker emits, what's left for game-load"
metadata:
  node_type: memory
  type: project
  originSessionId: 4a87fe3d-0fa1-4aac-8adf-aa87c4c6e1de
  modified: 2026-11-11T20:18:33.800Z
---

Map compiler: `src/zone/zonewriter.{h,cpp}` (stream model) + `src/zone/zonebuild.{h,cpp}` (assets).
CLI: `iw4r_zone compile <map.map> <out.ff>`. API: `compileMapToFastfile(doc, name, out, err)`.
Emits ComWorld(0x10), GameWorldMp(0x12), FxWorld(0x14), MapEnts(0x13); also buildRawFile(0x24).
Builders fill the real x64 structs from src/game/iw4_structs.h (included inside `namespace iw4`).

**blockSize SOLVED 2026-11-09** by reversing DB_LoadXFile headless in IDA (see [[reference-iw4-symbols]]).
Loader model (addresses image-relative in the .i64; full list in zonewriter.h header comment):
- each asset: Load_XPtr does push(TEMP), align4, [-2 header: DB_InsertPointer = 8-byte slot in VIRTUAL,
  align 8], top-level struct into TEMP, sub-data under push(VIRTUAL). Pop of TEMP rewinds it.
- blockSize[TEMP] = 32 (XAssetList, linker convention) + max struct; VIRTUAL = asset array + sub-data
  with per-field alignment (DB_AllocStreamPos mask). Load_Stream has NO bounds check.
- file stream is packed; alignment and insert slots are memory-only.
- Proven: stock patch_code_pre_gfx.ff and code_pre_gfx.ff (rawfiles) rebuild BYTE-IDENTICAL incl blockSize
  (test testRebuildStockRawfileZones). Old model (TEMP=0) would have crashed the loader.
- Back-ref pointer = ((block<<28)|offset)+1 (note the +1); a -1/-2 pointer = inline follow.
- No script strings -> stringCount 0, strings ptr 0 (stock convention; old zones used 1 null entry).
- Block 6/7 = VERTEX/INDEX are D3D buffers; blocks 0,2-5 one alloc, 1,6,7 another, page-rounded.

**clipMap_t DONE 2026-11-09** (src/zone/clipmap.{h,cpp} = brushes -> arrays; zonebuild buildClipMap = serialize).
- Load_clipMap_t 0x1242b0 order != struct order; reconstructed order validated on boneyard (SP map, type 14):
  walking its clipMap lands exactly on its inline MapEnts. Boneyard clipMap at inflated offset 135361028.
- MapEnts is NOT a standalone asset in stock map zones: clipMap.mapEnts = -2, loaded inline (nested Load_MapEntsPtr,
  pushes TEMP inside clipMap). Stock zone order: ComWorld .. FxWorld .. GfxWorld, GameWorld, clipMap.
- Element pointers (side->plane, node->plane, brush->sides/baseAdjacentSide, leafbrushnode->brushes) are
  back-refs; -1 there loads ONE element inline. Stock clipMap planes back-ref GfxWorld's planes.
- Conventions + brush-edge adjacency format documented in clipmap.h. BSP: 1 node, leaf0 empty,
  leaf1 = world; its brushes are a leaf-brush-node kd-tree (clipmap.cpp buildLeafBrushTree).
- Leaf-brush-node tree REVERSED from boneyard data 2026-11-11 (was the open "not reversed yet"):
  cLeafBrushNode_s: axis@0, leafBrushCount(s16)@2 (>0 = leaf with that many brushes; <=0 = interior),
  contents@4, data@8. Leaf: data.leaf.brushes(u16*)@8 (back-ref into clipMap.leafbrushes). Interior:
  data.children = dist(float)@8, range(float)@12, childOffset[2](u16)@16/@18. **childOffset is RELATIVE**
  (child index = thisIndex + childOffset[k]); child0 = min side (brush min[axis] <= dist), child1 = max
  side. Stock DUPLICATES straddling brushes into both children (boneyard: 40630 leafbrushes vs 19144
  brushes, ~2.1x) -> a trace reaching a child has every brush overlapping that half, so collision is
  correct regardless of the exact range predicate (which I did NOT need to reverse). Builder: median-of-
  centres split on widest axis, leaf target 1024 (<<32767 cap), straddlers duplicated. Max brushes 65535
  (leafbrushes is u16). zonebuild serializes interior data.children; zonesim already loaded the tree.
  Validated by testLargeClipMapTree (40000 brushes -> tree, every brush reachable, loads). NOT yet
  confirmed in-game (walk the map, check no fall-through); if wrong, suspect child0/child1 direction.
- src/zone/zonesim.{h,cpp}: offline port of the loader (bounds-checked blocks, resolves back-refs into real
  memory) -> tests inspect clipMap_t as the game sees it; `iw4r_zone verify <ff>`; compile self-verifies.
  zonesim shares my reading of the loader, so it proves consistency, not game truth.

**GfxWorld DONE 2026-11-09** (src/zone/gfxworld.{h,cpp} data; zonebuild buildGfxWorld; zonesim port).
Load_GfxWorld = 0x11ae60 (header layout matched every loader offset). Zone now = ComWorld(+sun lights),
FxWorld, GfxWorld, GameWorldMp, clipMap(+MapEnts), rawfile maps/mp/<map>.gsc (zlib; game runs its main()).
All world assets named "maps/mp/<map>.d3dbsp" (the name the game looks up).
- Boneyard GfxWorld at inflated 35109385; surfaces array at 108261703 (found by record-shape scan). Forward walk
  only reaches materialMemory (inline materials/techsets, XModels in smodelDrawInsts make full walks costly).
- Conventions (documented in gfxworld.h): vertex 44B + PackedUnitVec (byte=v*127+127, w=63); dpvs node u16
  encoding; sortedSurfIndex 2 halves; surfaceMaterials + vis data + many arrays are RUNTIME (game-built);
  light grid row RLE (cols,zCount,zStart)/(cols,0) with 32x32x64 cells +131072; lightmap L8/A8R8G8B8.
- Light grid (uniform gray placeholder) fills EVERY cell of its box (stock only stores cells near geometry), so
  size = cols*rows*zs entries (4 B each). Over the full brush AABB of an imported stock map (huge sky/caulk
  shell) that was billions of entries -> VIRTUAL block overflowed 32 bits ("block 3 overrun: need 8 GB" /
  "read past end of zone image"). Fixed 2026-11-11: region = DRAWN geometry (tool faces already excluded)
  trimmed to 2nd..98th percentile (lightGridRegion), plus a hard budget of 4M entries (~16 MB): shrink the z
  band first, then the XY window, centred. z-cell count is also a BYTE in the row RLE -> max 255 (the game's
  "map too tall for lightgrid"). Cells outside the box are simply uncovered. In-game behaviour for lookups
  outside the grid is NOT verified (assumed clamp/fallback; if models far from the map centre render black,
  that's why). Regression: testImportFastfile recompiles imported mp_highrise and loader-checks it.
- External assets referenced by ','-prefixed name stubs (DB_AddXAsset 0x12e700 resolves or defaults them).
  Materials loaded once via materialMemory; surfaces back-ref insert slots. Images inline (tiny uniform).
- GfxWorld owns planes; clipMap back-refs them (stock). Checksums equal.
- Simulator caught a real bug (MSVC right-to-left arg eval moved a vector before size()).

**Map import DONE 2026-11-09** (src/zone/mapimport.{h,cpp}; File > Import Map from .ff; `iw4r_zone import`).
- Own zones: full simulateLoad. Stock SP/SpecOps zones: zone::simulatePartial loads GfxWorld + clipMap from their
  struct offsets (found via worldspawn lump -> MapEnts name back-ref -> same-name structs), lenient back-refs,
  VIRTUAL anchors (GfxWorld at planes = clipMap.planes ref; clipMap at brushsides = min raw brush.sides ref).
  clipMap candidate confirmed by its walk landing on the MapEnts offset. Boneyard: 19144 brushes, 24369/24369
  surfaces with materials, 258 colorMap images, ~2s. Signed MP zones import too (see [[signed-zone-format]]).
- 2026-11-11 sweep: all 20 campaign zones + all 24 Spec Ops zones import. 22 SO zones are OVERLAYS
  (so_intel_boneyard.ff = 7 MB): no GfxWorld/clipMap, just a standalone `maps/<mission>.mapents` with an
  INLINE name (the "name isn't a back-reference" case) whose struct is 0x48 bytes (not 0x58 like the
  clipMap-nested one; locate the lump via numEntityChars@16, don't assume the size). Overlay lumps are
  ADDITIVE (so_ac130_co_hunted has 6 ents: no lights/nodes/spawners) -> importOverlay = base zone (longest
  `_suffix` of the file name that exists beside it: so_crossing_so_bridge -> so_bridge) + mission ents,
  overlay worldspawn keys win. so_bridge/so_ghillies are standalone full maps.
- Still not imported from stock zones: static models (sim stops at GfxWorld.dpvs.smodelDrawInsts) and
  clipMap terrain/patch collision (only brushes are converted) and dynEnts.
- Simulator now has full Material/techset/technique/pass/shader/vertexdecl/water loaders + MapEnts triggers/stages.
- NAMING: editor/.map material names have NO "wc/" prefix (AssetDb keyed that way). Compiler references
  ",wc/<name>" for bare names; clipMap names are bare; importer strips "wc/" from GfxWorld names.
- Editor: RefMesh (read-only render mesh) on Document, drawn with depth bias; View menu toggles; tool faces hidden
  after import; camera starts at player start. Possible perf issue: renderer rebuilds all brush tris on every
  doc revision (selection too) - slow with 20k-45k imported brushes; not measured.

NOT verified in-game; renderer guesses to check first: triangle winding/culling, lightmap/probe semantics,
vis-data counts, fogTypesAllowed, delayLoadPixels, missing materials -> default. Next: in-game test via the
user's client, then real materials/lighting. Also brush-entity submodels, patches, static models, sky.

**How to apply:** every new asset builder must mirror its Load_* traversal (order, push blocks, align
masks) exactly — get it from the decompile, not from community 32-bit docs.
