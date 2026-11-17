---
name: client-integration
description: How the user's private IW4 client (Serenity) loads custom assets - the integration path for iw4-radiant maps
metadata:
  type: project
---

The user runs a private x64 IW4 client ("Serenity"), injected by `C:\Users\Lurkz\Desktop\IW4-Launcher64` (launcher source is there; the injected client is separate and compiled). Decided 2026-11-04 to load custom maps via **runtime asset injection through the client's hooks**, not by defeating any check.

Key evidence (read-only, from the MW2 folder and desktop):
- The client already **accepts unsigned zones**: `serenity_controller_common.ff` is plain `IWffu100`, version 0x114 (276), zlib stream from offset 21 - the same unsigned format iw4-radiant's `fastfile.cpp` already reads. Its `asset_hooks_debug.log` shows it hooks `DB_ValidateZoneMagic`, `Sys_CreateFile`, `_open/_read/_lseek/_close`, `ReadFileEx` and redirects that custom ff in by filename. So custom unsigned zones load by the client's own design.
- The client already **builds assets in memory at runtime**: `runtime_materials_debug.log` shows it constructs Material + GfxImage + GfxTexture structs from scratch (controller icons). Proves the in-memory asset-injection approach works on this build.
- **The zone format question is answered**: output unsigned `IWffu100`, exactly what iw4-radiant reads. No signing needed.

Assets on disk that help (see [[reference-iw4-symbols]], [[x64-zone-format]]):
- `C:\Users\Lurkz\Desktop\IW4Controller-x64` - a working injected-DLL mod for this build (dllmain, hooks/hook_manager, engine/addresses.h, docs/ADDRESS_MAP.md with RE'd x64 addresses, imagebase 0x140000000). Model iw4-radiant's game-side loader on this.
- `C:\Users\Lurkz\Desktop\example_sdk_mod` - the client's mod SDK: `mod.dll` + `mod.json` + GSC `scripts/entry.gsc` exposing GSC builtins (example_add/scale/greet/info). GSC route, not a C++ asset API - use the injected-DLL route for world assets.
- `C:\Users\Lurkz\Desktop\t6sp-hub\oat-windows` - OpenAssetTools (Linker/Unlinker/ImageConverter). Can't build IW4 world assets, but useful for reading/other games.

**How to apply:** the remaining work is the compiler: build GfxWorld/clipMap_t/ComWorld/GameWorldMp/FxWorld/MapEnts in the x64 layout and link an IWffu100 zone named mp_<name>.ff. Needs x64 struct layouts RE'd from `iw4mp_2013.exe.i64`. Good first milestone: round-trip serenity_controller_common.ff through iw4-radiant's reader+a new writer and confirm the client still loads the rebuilt copy, to validate the container before world assets.

**Round-trip milestone (done 2026-11-04):** iw4-radiant can read and rebuild the client's unsigned zone format losslessly (src/fastfile.cpp, iw4r_zone CLI). A rebuilt serenity_controller_common.ff is in the session scratchpad for the user to drop in and confirm the client loads it. Next: build world assets (clipMap_t/GfxWorld/etc.) in the x64 in-memory layout, RE'd from iw4mp_2013.exe.i64.
