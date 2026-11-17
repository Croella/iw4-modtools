---
name: reference-iw4-symbols
description: "Where IW4 symbols, IDA databases, and game data live on this PC, and which are safe to trust for layouts"
metadata:
  node_type: memory
  type: reference
  originSessionId: 4a87fe3d-0fa1-4aac-8adf-aa87c4c6e1de
  modified: 2026-11-11T18:15:16.953Z
---

- `C:\Users\Lurkz\Desktop\iw4mp_2014.exe`: the CURRENT game build (2.0.14), a 156MB unpacked dump (live game is the packed 6.25MB Steam iw4mp.exe). No .i64 yet; RE it with a capstone script on the raw PE (sections: .text va 0x1000). Struct layouts match 2013 (zone version 276 unchanged); only global addresses/code move between updates. See [[live-process-dump]].
- `C:\Users\Lurkz\Desktop\iw4mp_2013.exe` and `.i64`: PREVIOUS build (now outdated for live addresses). x64 binary with IDA database. Still the right reference for struct layouts.
  The .i64 has NO function names and is rebased to 0 (image-relative eas); find code via string xrefs.
  Reverse headless with IDA 9.2 idat.exe (Hex-Rays x64 works): scripts in iw4-radiant `tools/ida/`
  (run.sh + dec/dis/strx/dw.py); always run on a COPY of the .i64. Hex-Rays hides push-block args: use dis.py.
  Load_XAsset switch at 0x122e30 maps type -> Load_*Ptr (clipMap 14/15 -> 0x1241f0, GfxWorld 21 -> 0x11c310).
  tools/ida/decpush.py = decompile with push-block args restored (PUSH(n)) + helper names; finddisp.py = find
  functions by struct-field displacements (noisy for small offsets). Scratch offset probe: a tiny CMake project
  including src/zone/iw4.h printing offsetof/sizeof (vcvars from bash doesn't work; use cmake).
- `C:\Users\Lurkz\Desktop\iw4mp pdb\`: **Xbox 360** binaries (XEX2, load 0x82000000, big-endian, 32-bit) with PDB and .map files, plus `2-iw4mp.exe.h`, an IDA header with world structs (GfxWorld around line 96676, clipMap_t around line 95836). Good for field names only. Layouts differ (GfxImage, Material, XAssetType all differ from the x64 zones).
- MW2 install: `C:\Program Files (x86)\Steam\steamapps\common\Call of Duty Modern Warfare 2`.
- Black Ops install has `linker_pc.exe` and `asset_manager.exe`, but no Radiant or BSP compiler, and the `map_source` .map files sampled are blank launcher stubs, so there is no real .map sample on this PC.

Related: [[project-direction]], [[x64-zone-format]].
