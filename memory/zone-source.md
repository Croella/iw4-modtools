---
name: zone-source
description: "Custom asset pipeline (zone_source next to the .map): xmodel_export -> XModel, textures embedded; RE'd XModel/XSurface load order"
metadata:
  node_type: memory
  type: project
  originSessionId: c635133b-e9c7-4023-9bad-d8499390d538
  modified: 2026-11-12T01:08:25.140Z
---

User chose (2026-11-11): source format = xmodel_export/xanim_export (Blender/Maya CoD plugins),
folder = `<map folder>/zone_source/` (per-folder, auto-picked when compiling a map there).
Plan: M1 static models (DONE, VERIFIED IN-GAME 2026-11-11: demo crates show in iw4sp) -> M2
xanim_export -> XAnimParts + skinned models -> M3 weapons.

**Layout:** zone_source/xmodels/<name>.xmodel_export (asset name = stem), zone_source/images/
<tex>.png|.tga (embedded A8R8G8B8, 1 level, category 2, flags 2 like lightmaps; else streamed from
IWDs by texture stem). Place with script_model ("model" key); misc_model using a zone_source
model -> script_model in the entity lump. Code: src/model/{xmodelexport,imageio}, src/zone/
modelbuild (pack GfxPackedVertex, half uv, winding = cross(b-a,c-a) opposite the normal),
zonebuild buildXModel/planModelMaterials/loadZoneSourceModels, CompileOptions::zoneSourceDir.
Editor: App::loadZoneSourcePreview (on open + File > Reload zone_source Models), Renderer::
setTextureFile. Demo: maps/modeltest.map + maps/zone_source (demo_crate), deployed as
zone/english/modeltest.ff.

**RE (iw4mp_2013.i64, decompiled with tools/ida decpush.py on a COPY):** Load_XModelPtr 0x123700,
Load_XModel 0x123260 (408, VIRTUAL): name; boneNames u16 script-string idx (align2); parentList
(nb-nr, a1); quats 8*(nb-nr) a2; trans 12*(nb-nr) a4; partClassification nb; baseMat 32*nb a4;
materialHandles (non-null) 8*numsurfs + Load_MaterialHandle each; lodInfo[4] inline (56 each),
each modelSurfs -> Load_XModelSurfsPtr 0x123980 with lod.numsurfs; collSurfs 48 (+collTris 48);
boneInfo 28*nb; physPreset (0x11e670: 56, name, +32 sndAliasPrefix); physCollmap (0x11e4c0).
XModelSurfs 0x1238c0 (48): name; surfs 88 each -> 0x123bb0: vertsBlend 2*(c0+3c1+5c2+7c3) a2;
PUSH VERTEX verts0 32 each a16; vertList 16 each (+collisionTree 56: nodes 16 a16, leafs 2 a2);
PUSH INDEX triIndices 6*triCount a16. Stock 1-bone template (so_intel_boneyard viewmodel_nvg):
lod dist 1000, rigid list {bone 0, all verts/tris, tree NULL}, partBits[0]=0x80000000,
baseMat identity transWeight 2, collLod -1. rigid boneOffset = bone*64. Model techsets in
common.ff: mc_l_sm_b0c0[n0][s0] (+modelLightingSampler). Material library now has mc/ too (MDF2).
zonesim supports types 0/3/4 + modelprobe CLI (prefix-loads stock zones, prints models).

**M2 (2026-11-11, built, loader-sim ok, NOT yet verified in-game):** multi-bone xmodel_export ->
skinned XModel (modelbuild: roots first, parentList, local quats/trans, baseMat bind pose,
vertsBlend groups, deformed=1, per-vertex summed tangents so verts dedupe ~stock counts).
zone_source/xanims/*.xanim_export -> XAnimParts (src/model/xanimexport, src/zone/animbuild:
<=256 frames, every frame keyed, full-quat / full-quat-no-size / trans u16 / trans-no-size /
no-trans; local pose from the zone_source model with most matching bones; stem ending
_loop/_idle -> flag 1 looping) + rawfile animtrees/custom.atr listing them, so scripts use
#using_animtree("custom") + useAnimTree(#animtree) + setAnim(%name, 1, 0, 1).
XAnimParts loader+evaluator layout validated with `iw4r_zone animprobe <zone.ff>` against
stock juggernaut anims (so_takeover_oilrig.ff): keys frames 0..numframes, numframes = frames-1.
Demo: tools/demo_tail.py generates demo_tail (4-bone column) + demo_tail_loop; maps/modeltest.gsc
plays it on 3 script_models (targetname demo_tail); deployed modeltest.ff. User also put a CoD4
viewhands_op_force.xmodel_export (71 bones) in zone_source; maps/basic.gsc sets it as viewmodel.
Next: in-game confirm, then M3 weapons.

**Skinning RE facts (2026-11-11, from in-game bugs):** bone names must be lowercase (stock are;
anims match by exact script string -> mixed-case CoD4 rig = invisible hands). Skeleton build
iw4mp 0x2ada80: world = parent + rot(parent, scale*animTrans + XModel.trans[bone]) -- model trans
is ADDED; unanimated bones get model quat + zero anim trans (0x2b8470). So stock viewhands store
trans = 0 (weapon anims carry full offsets; characters store real local trans). We zero trans for
models named viewhands*/viewmodel* (Bone::modelTrans) and animbuild stores trans minus modelTrans.
CPU skinning 0x705d0 etc confirms vertsBlend [b0 b1 w1 b2 w2], bone = byte offset (x64), w/65536.
Our CoD4 viewhands bind pose == stock MW2 viewhands bind pose minus z 60 (root tag_view vs stock
tag_origin->tag_view). Probe: IW4R_BONES=1 IW4R_ALLBONES=1 iw4r_zone modelprobe <ff>.
