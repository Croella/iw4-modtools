---
name: brush-triggers
description: "Brush triggers (trigger_multiple etc.) compile to MapEnts.trigger '?N' models (stock format RE'd from boneyard/favela); other brush entities are brush models '*N' (see world-geometry)"
metadata:
  node_type: memory
  type: project
  originSessionId: c635133b-e9c7-4023-9bad-d8499390d538
  modified: 2026-11-15T23:18:27.866Z
---

Added 2026-11-15 (user: "make it target brush entities").

**Stock ground truth** (dumped from mp_boneyard, boneyard, favela): brush triggers are NOT "*N"
cmodels. Their entity has "model" "?N" = MapEnts.trigger.models[N], numbered in entity order.
TriggerModel{contents, hullCount, firstHull}; one TriggerHull per brush (AABB relative to the
entity origin, contents, slabs); TriggerSlab = one per non-axial face direction (parallel faces
share one), dir + mid/half of the brush's extent along it, relative to the origin. Contents vary in
stock (0x28000001 / 0x28000000 / 0x28004000, even within trigger_multiple); we use 0x28000001.
info_volume and stage also use "?N". script_brushmodel uses "*N" cmodels (leaf -> leafBrushNode
listing its brushes, bounds local): compiled since 2026-11-16, see [[world-geometry]].

**Code:** clip::buildTriggers/addTriggerModel/isBrushTriggerClass (src/zone/clipmap.*), arrays in
ClipMapData.trigger*, written by writeMapEnts (zonebuild.cpp, Load_MapTriggers order after the
entity string). Origin = rounded brush centre unless the entity has one. testBrushTriggers.
NOT yet verified in-game (loader sim only).

Related: [[effects]], [[zone-linker]], [[x64-struct-layouts]].
