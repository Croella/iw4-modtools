---
name: world-geometry
description: "Patches, brush models (*N), static models (misc_model) and XModel collision: what the compiler emits and the stock formats RE'd from mp_rust/boneyard dumps"
metadata:
  node_type: memory
  type: project
  originSessionId: c635133b-e9c7-4023-9bad-d8499390d538
  modified: 2026-11-16T22:02:15.439Z
---

Added 2026-11-16 (user: "do those" = patches, static misc_models, script_brushmodels, model collision).
Shared decisions live in src/worldgeom.{h,cpp} (brushModelEntities/Origin, worldPatches, entityPlacement,
dopBrush 26-DOP hull) + src/patch.{h,cpp}; bake, gfxworld, clipmap and entity lump all use them.
NOT verified in-game (loader sim + tests only).

**Stock ground truth** (IW4R_DUMP_SMODELS=1 iw4r_zone import <stock.ff>; zonesim now loads
smodelDrawInsts + inline XModels in lenient mode, stops at the first physCollmap):
- GfxStaticModelDrawInst is **88 bytes on x64** (model ptr @56). flags 0 (0x20 = ground lighting),
  lightingHandle/cacheId runtime, cullDist = scale * last LOD dist (0 if "infinite" 1e6).
  smodelInsts bounds = world AABB, lightingOrigin = centre. smodelVisDataCount = ((n+127)>>7)<<2.
  aabb tree root lists every smodel index.
- clipMap staticModelList only for models with collision; invScaledAxis[r][c] = axis[c][r]/scale.
  smodelNodes: child >= numStaticModels -> node (child - numStaticModels), else static models
  (mp_rust leaves list all 2315 once). Bullet/sight traces only; players need clip.
- XModelCollTri_s: plane(n,d), s = dot(svec.xyz,p) - svec.w, t likewise (barycentric along b-a,
  c-a; tri wound clockwise from outside). surfFlags = surfaceType<<20, contents 1 solid / 2 foliage.
- Brush models: GfxBrushModel/cmodel index N = "*N"; geometry + brushes origin-relative; surfaces
  after the static ones (staticSurfaceCount = world only), lightmapped, primary light 0;
  invisible ones startSurfIndex 65535. cmodel bounds = brush bounds +1, leaf -> leaf-brush node.

**Compiler:** patches tessellated (curve: adaptive bezier) -> Face with uvs/vnormals/colors,
collision = 4-unit prism brushes (nonColliding skipped); winding front = cross(d/dcol, d/drow)
(from corvid's Patch.Triangulate, UNVERIFIED). Patch "t" = uv * texture size (colorMap image
size from matdefs, else 512). Brush-model faces baked with the sun included (LitFace::bakeSun).
misc_model -> static model (by-name ",model" XModel stub, slot shared with clipMap); custom models
get occluder tris in the bake, clip_player brush per "collision" key (hull default/box/mesh/none)
and a hull collSurf on the XModel; misc_models with targetname etc. stay script_models. Custom
XModel LOD0 dist now 1e6 (was 1000: would vanish at 1000 units). Stock models: no geometry
(64-unit box bounds, no collision; warned).

Related: [[brush-triggers]], [[zone-linker]], [[lighting]], [[zone-source]].
