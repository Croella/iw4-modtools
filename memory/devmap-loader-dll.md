---
name: devmap-loader-dll
description: "iw4r_mapload.dll - the editor's own injectable DLL that makes compiled maps devmap-able by redirecting the zone file open"
metadata:
  node_type: memory
  type: project
  originSessionId: 5594d009-4322-40bf-84d3-5c0c822a3f25
  modified: 2026-11-11T19:31:27.605Z
---

`iw4r_mapload.dll` (src/dll/mapload_dll.cpp; CMake target `iw4r_mapload`, SHARED, x64, built next
to iw4radiant.exe) lets the editor's compiled maps load via `devmap`. Added 2026-11-11. Standalone
(user's choice, NOT built into IW4Controller-x64 or Serenity).

**Key finding:** the 2.0.14 loader ALREADY accepts unsigned IWffu100 zones (inline magic compare at
RVA 0x117e8c / 0x12b467), so NO signature/validation bypass is needed. The only missing piece for a
custom map is that the game opens `mp_<name>.ff` from its own install, not the editor's deploy
folder. So the DLL just REDIRECTS the file open (benign; touches no game memory/code).

**How:** DllMain spawns a thread that reads the deploy folder from `%LOCALAPPDATA%\iw4-radiant\
launch.cfg` (same file the editor writes), then IAT-hooks CreateFileA/CreateFileW across all loaded
modules (re-applied ~5 min to catch late modules; applied hooks persist). In the hook: if the opened
path ends in `.ff` and that basename exists in the deploy folder, open the deploy copy instead.
Stock zones (not in deploy folder) are untouched. Logs to `%LOCALAPPDATA%\iw4-radiant\mapload.log`.

**Injection:** the editor's Compile & Run (F5, see [[compile-and-run]]) launches iw4mp.exe plain
(no +devmap), then injects this DLL (CreateRemoteThread+LoadLibraryA, App::injectDll). User then
runs `devmap mp_<name>` from the in-game console.

**Two hook layers (2026-11-11), no game addresses, survives patches:**
1. IAT patch of CreateFileA/W in every module — INCLUDING kernel32 itself: on Win10 19045
   kernel32!CreateFileA/W are `FF 25` `jmp [slot]` stubs through kernel32's own IAT to KernelBase,
   so patching that slot also catches the packed game's GetProcAddress'd calls.
2. Inline hook on KernelBase!CreateFileA/W (14-byte `FF 25` abs jmp + trampoline), only if the
   first 15 bytes match the prologue seen on this build (A: `48 89 5c 24 08 48 89 74 24 10 48 89 7c
   24 18`, W: `48 83 ec 58 44 8b 94 24 88 00 00 00 41 8b c2`, all position-independent); other
   threads suspended while patching, threads parked mid-prologue moved into the trampoline.
**Recursion traps (both hit and fixed):** (a) the "real" function must be KernelBase's, never the
kernel32 export (its slot is patched -> hook calls itself); (b) publish the trampoline as the real
pointer BEFORE writing the jmp — the hook fires immediately (even from the DLL's own fopen logging).
CRT `_open`/fopen goes through CreateFileW, so it's covered too. Env overrides IW4R_DEPLOY_DIR /
IW4R_MAPLOAD_LOG exist for testing.

**Status: offline-verified, NOT yet confirmed loading a map in-game.** Test harness loads the DLL
into a python process (scratchpad dlltest/run.py): all 8 paths (CRT open, kernel32 A/W, KernelBase
A/W redirect; stock .ff + non-.ff untouched) pass. In-game: F5, `devmap mp_<name>`, check mapload.log
for "redirect ... -> <deploy>\mp_<name>.ff". See [[zone-linker]] (the compiler that makes the zone).

**Loader DLL was never in the real game until 2026-11-13:** Steam relaunches iw4sp/iw4mp, so the injected first process exits. Compile & Run now follows the relaunch (see [[volumetric-lighting]]).
