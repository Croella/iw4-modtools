---
name: x64-struct-layouts
description: "Validated x64 IW4 asset struct layouts for the map compiler, their source, and how to regenerate/verify"
metadata:
  node_type: memory
  type: project
  originSessionId: 4a87fe3d-0fa1-4aac-8adf-aa87c4c6e1de
  modified: 2026-11-10T00:07:32.847Z
---

Mapped the x64 world-asset struct layouts 2026-11-04. Source: the RE'd header in the user's own
project `C:\Users\Lurkz\Desktop\SerenityCheats\iw4mp-controller\iw4mp-controller\structs.h`
(11,954 lines, targets this exact x64 build; a near-duplicate is in simasce_carrera-iw4_controller/Structs.h).
Its fields use real C++ pointers, so compiled x64 the pointers are 8 bytes = the x64 layout. Its
own 65 static_asserts are STALE 32-bit checks (e.g. sizeof(CmdArgs)==0x84 needs 4-byte pointers)
and must be ignored/stripped.

Validated against real zone bytes (ground truth, not the stale asserts):
- XFile = 0x28 -> zone stringCount@40, assetCount@56  (matches boneyard.ff and fastfile.cpp)
- Material = 0x78, textureCount@0x50, techniqueSet@0x58, textureTable@0x60; MaterialTextureDef = 0x10
  (matches the working scanMaterials on real zones)
- MapEnts = 0x58: name@0, entityString@8 (-1 inline), numEntityChars@0x10, trigger(MapTriggers 0x30)@0x18,
  stages@0x48, stageCount@0x50  (confirmed at boneyard zone offset 0x89d5eed: numEntityChars=618896=entstr len+1)
- clipMap_t = **0x200** (Load_clipMap_t reads 0x200; the RE'd header said 0x1c0 - fixed by adding
  unknown5[0x30] after checksum, zero in boneyard). DynEntityClient = 16 on x64 (header had 12; physObjId
  made __int64). LESSON: sizes "validated" only by compiling the header are NOT validated - check each
  struct against its Load_* Load_Stream size before relying on it (GfxWorld 0x3b0 is still unchecked).
- ComWorld = 0x18; GameWorldMp = 0x10; FxWorld = 0xb0 (all confirmed by their Load_* sizes)
- collision subs: cplane_s 0x14, cbrush_t 0x30, cbrushside_t 0x10, cmodel_t 0x44, cLeaf_t 0x28, cNode_t 0x10, cStaticModel_s 0x50
- GfxImage = 0x28 (name@0x20)

Committed as `src/game/iw4_structs.h` in iw4-radiant: prelude (forward-decls for D3D9 + Windows
handle types, used only as pointers) + extracted struct bodies (lines 2..6120 through XAssetList,
stale asserts stripped) + x64 static_asserts validated above. `tools/structcheck.cpp` /
`iw4r_structcheck` target compiles it x64 so the build fails if any layout drifts.

To re-dump offsets from scratch: scratchpad/layout had probe.cpp (forward-decls + XINPUT/in_addr/
tagPOINT stubs + `#define static_assert(...)`) built via a tiny CMake project.

**How to apply:** the compiler writes these structs into zone memory blocks with 0xFFFFFFFFFFFFFFFF
for follow-pointers and back-references encoded ((block<<28)|offset)+1 (verified in DB_ConvertOffsetToPointer, see [[zone-linker]]), matching the XAssetList serialization. GameWorldMp/ComWorld/FxWorld are trivial
(mostly name + zero/null). clipMap_t (collision) and GfxWorld (render + lightgrid + dpvs) are the
heavy ones. See [[x64-zone-format]] and [[client-integration]].
