---
name: compile-and-run
description: "Editor 'Compile & Run in Game' (F5): save -> compile -> deploy .ff -> launch iw4mp.exe +devmap"
metadata:
  node_type: memory
  type: project
  originSessionId: 5594d009-4322-40bf-84d3-5c0c822a3f25
  modified: 2026-11-11T19:21:22.069Z
---

iw4-radiant can live-test the open map in one step: **File > Compile & Run in Game (F5)**
(`App::compileAndRun` in src/app.cpp). Added 2026-11-11. Flow:
1. Save the .map (Save As dialog if it has no path yet).
2. Compile to an unsigned mp_<name>.ff and run the loader-sim check (`compileCurrentMap` helper,
   shared with compileDialog) — refuses to launch a zone the game would reject.
3. Copy it to the user's **custom-zone folder** as mp_<name>.ff. Folder is asked once via a folder
   picker and persisted to `%LOCALAPPDATA%\iw4-radiant\launch.cfg` (load/saveLaunchConfig); editable
   in Settings ("Custom-zone folder (F5 deploy)" + Browse).
4. Launch `<gameInput>\iw4mp.exe` with NO command-line options (user said not to use launch options,
   2026-11-11 — also avoids the zone load racing the hooks), cwd = game folder. gameInput = "Game
   folder" in Settings.
5. Inject `iw4r_mapload.dll` (next to the editor exe) via CreateRemoteThread+LoadLibraryA
   (App::injectDll). Then the user opens the console (~) in-game and runs `devmap mp_<name>`.

**Decisions (user, 2026-11-11):** launch iw4mp.exe directly (NOT the Serenity launcher), no launch
options; deploy to a user-specified custom folder (NOT auto to zone/english); the editor injects
its OWN loader DLL (NOT Serenity).

**The loader DLL = [[devmap-loader-dll]]** (iw4r_mapload): makes the editor's unsigned zones
devmap-able by redirecting the file open. No signatures/game memory touched (loader already accepts
IWffu100). See [[zone-linker]] (compiler), [[live-process-dump]].

**MP/SP target (2026-11-11):** F5/F7 + their menu items open an ImGui modal ("Compile for":
Multiplayer (iw4mp) / Singleplayer (iw4sp), remembers last pick, Enter = last, Esc = cancel) via
App::requestCompile/drawCompileTargetPopup, then compileDialog(MapTarget)/compileAndRun(MapTarget).
SP: deploys <name>.ff, launches iw4sp.exe, same loader DLL (address-free, should work unchanged).
compileMapToFastfile(..., MapTarget::SP) -> maps/<map>.d3dbsp, clipmap_sp (0xE), GameWorldSp (0x11,
0x70-byte struct, null pathdata; Load_GameWorldSp identical in SP 0xDDD70 / MP2013 0x11A360),
maps/<map>.gsc = maps\_load::main(). CLI: `iw4r_zone compile --sp`. Unit test testZoneCompileSp
passes the loader sim. In-game SP: got past zone load to a crash (fixed below); next run untested.
**SP crash fixed (2026-11-11):** first SP test crashed silently at iw4sp+0x16ad56 (glass init reads
g_glassData->pieceCount, no null check in SP). GameWorldSp now carries an EMPTY G_GlassData (0x90,
align 4; Load_G_GlassData SP 0xdda40). Crash dumps land in %LOCALAPPDATA%\CrashDumps\iw4sp.exe.*.dmp;
the SP dump exe's RVAs match the live game (base 0x7FF7C2DC0000) — analyze with python `minidump`
+ capstone on C:\Users\Lurkz\Desktop\iw4_2014_sp.exe. User's deployDir = zone/english itself.
**SP crash #2 fixed:** iw4sp+0x8607 reads cm.mapEnts->stages[stageIdx] (stage lookup iw4sp+0x46c20
returns 0 when in no stage trigger) with no null check. SP MapEnts now gets one stage 0
("default", sunPrimaryLightIndex 1) via buildClipMap(..., withDefaultStage). Pattern: SP drops
null checks MP has — when SP crashes on a null, emit a minimal non-null struct.
SP RE landmarks (2.0.14 dump RVAs): cm (clipMap_t copy) = 0x131F800 (mapEnts @0x131F930), addon
mapents 0x131F780, entity parse point 0xD021E0, G_ParseSpawnVars 0x170690, SP_worldspawn 0x19AA00,
CM_LoadMap 0x1F0220 (looks up type 0xE). Live-state reader: scratchpad entstate.py (per session).

**Custom map script (2026-11-11):** `<map>.gsc` next to the .map (mapScriptPath/readMapScript in
zonebuild) replaces the generated maps/[mp/]<map>.gsc (CompileOptions::mapScript). Editor: File >
Edit Map Script (creates from template for the last compile target, opens in the .gsc handler or
Notepad); compile logs which script it used. CLI compile picks it up too. User chose this over hot
reload / in-editor script window (2026-11-11).

**Auto-copy to zone/english (user request, 2026-11-13):** every compile (F5 and F7) also writes <map>.ff + <map>.volumetric to <game>/zone/english (App::deployToGameZone), so the game finds the map even without the DLL. The DLL activates volumetrics for an in-place .ff that has a .volumetric next to it.
