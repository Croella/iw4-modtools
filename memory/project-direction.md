---
name: project-direction
description: iw4-radiant goals and decisions: Radiant-style IW4 editor targeting the user's x64 iw4mp build, own compiler, editor first
metadata:
  type: project
---

iw4-radiant is a Radiant-style map editor for IW4 (MW2). Decisions made 2026-11-04:
- Target build: the x64 iw4mp.exe in the user's Steam MW2 folder, not IW4x.
- Compile route: write our own compiler that outputs IW4 world assets (clipMap_t, GfxWorld, ComWorld, GameWorldMp, FxWorld, MapEnts). OpenAssetTools can't build these and no CoD4 tools are installed.
- Order: editor first, then the in-game pipeline.
- Stack: C++20, CMake, MSVC (VS 2026), D3D11, Dear ImGui docking.

**Why:** the community route (cod4map, iw3xport, ZoneBuilder) needs CoD4, Mod Tools and IW4x, and may not produce zones the x64 build loads.

**How to apply:** check new work against the x64 target. Treat community IW4 docs as 2009/32-bit references and verify them against this build. See [[reference-iw4-symbols]].

2026-11-16: the user wants Radiant to be a secondary tool; iw4modtools.exe is the main program (see [[asset-manager]]).
