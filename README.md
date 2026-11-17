# iw4-modtools

## Build

Requires Visual Studio 2026 with the C++ desktop workload, and CMake 3.24 or newer. ImGui and
miniz are downloaded at configure time.

```sh
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

## Run

```sh
build/Release/iw4radiant.exe                # empty map
build/Release/iw4radiant.exe my_map.map     # open a map
build/Release/iw4radiant.exe --demo         # generated sample room
build/Release/iw4radiant.exe --game "D:/path/to/mw2"
```

The first launch scans the zones in the background, which takes about 3 minutes. Results are
cached in `%LOCALAPPDATA%\iw4-radiant\materials.cache`, so later launches start immediately.
Delete that file to force a rescan. The game folder can be changed in **Edit > Settings**.

## Controls

| Where | Input | Action |
| --- | --- | --- |
| Grid | Drag on empty space | Create a brush |
| Grid | Click | Select what's under the cursor (smallest first) |
| Grid | Shift+click | Add to or remove from the selection |
| Grid | Drag inside selection | Move |
| Grid | Drag selection edge or corner | Resize |
| Grid | Right-drag | Pan |
| Grid | Right-click | Add a point entity |
| Grid | Wheel | Zoom at the cursor |
| Grid | Middle-click | Move the camera here |
| Camera | Hold right mouse + WASD, Q/E | Fly (Shift is faster) |
| Camera | Wheel | Dolly |
| Camera | Click, Shift+click | Select, add to selection |
| Textures | Click | Set the current material and apply it to the selection |

| Key | Action |
| --- | --- |
| Ctrl+Z, Ctrl+Y | Undo, redo |
| Ctrl+S, Ctrl+Shift+S | Save, save as |
| Ctrl+D | Clone |
| Ctrl+H | Hollow, with walls one grid unit thick |
| Ctrl+A | Select all |
| Del, Backspace | Delete |
| Esc | Deselect |
| `[`, `]` | Halve, double the grid |

## Status

Working:

- Convex brushes: create, move, resize, clone, hollow
- Sloped brushes load, render, and save, though there's no tool to cut them yet
- Point entities, brush entities, and a key/value inspector
- Undo and redo (64 steps, full snapshots)
- `.map` load and save, preserving patches and unknown lines verbatim
- Textured camera view and a searchable material browser, using your game's assets
- Reading and rebuilding unsigned (`IWffu100`) fastfiles losslessly, via `iw4r_zone` (see below)
- Reading signed (`IWffs100`) fastfiles — the retail multiplayer maps — by stripping their
  authentication blocks, so `mp_*` maps import and their materials show in the browser

Not done yet:

- Compiling to game assets: building world assets (`GfxWorld`, `clipMap_t`, `ComWorld`,
  `GameWorldMp`, `FxWorld`, `MapEnts`) in the x64 in-memory layout and linking them into a
  zone. The fastfile container around them is done; the assets inside are the biggest piece left.
- Vertex editing, a clipper, per-face texturing, and patch editing
- Drawing patches, which are kept in the file but not shown
- Showing XModels. `misc_model` and `script_model` appear as boxes.

## Known limitations

- **The `.map` format is unverified.** It's written from memory of the `iwmap 4` format because
  no real sample was available on this machine. Our own files round-trip byte for byte, but
  the texture fields in particular may not match real files.
- **About 17% of materials have no resolved colorMap.** The image is a back-reference that
  needs a real zone walk. These fall back to guessing from the name, then to a checkerboard.
- **Tool materials are placeholders.** `caulk`, `clip` and the others are guessed names that
  appear as coloured checkerboards.
- **The entity list is a starting set**, not a full definition of what the game accepts.

## Format notes for the compiler

The zones in this install are not the 2009 32-bit format:

- `iw4mp.exe` is x64. Fastfiles report version 276, but use **64-bit pointers on disk**
  (`0xFFFFFFFFFFFFFFFF` = follow; otherwise an offset-encoded back-reference, top nibble = block).
- The x64 asset struct layouts are mapped in **`src/game/iw4_structs.h`** and guarded at build
  time by the `iw4r_structcheck` target. They were validated against real zone bytes:
  `XFile`=0x28 (so `stringCount`@40, `assetCount`@56), `Material`=0x78 (`textureTable`@0x60),
  `MapEnts`=0x58 (`numEntityChars`@0x10), `clipMap_t`=0x1c0, `GfxWorld`=0x3b0, `ComWorld`=0x18,
  `GameWorldMp`=0x10, `FxWorld`=0xb0, plus all collision sub-structs.
- The `XAssetType` enum is the PC/x64 numbering (`CLIPMAP_MP`=0xF, `GFXWORLD`=0x15), not the Xbox
  one — see the enum in `src/game/iw4_structs.h`.
- The 2009 PDBs on this machine are Xbox 360 builds (`XEX2`, big-endian, 32-bit) — field names
  only, not layouts. `iw4mp_2013.exe.i64` is the x64 reference.
- **Signed (`IWffs100`) container:** after the file header comes a fixed `DB_AuthHeader` (RSA
  signature + 244 master-block hashes) and a `DB_MasterBlock` (256 SHA-256 chunk hashes), then
  the compressed data at `0x4015`. The zlib stream is interrupted every `0x200000` bytes by a
  fresh `0x2000`-byte master block (256 chunks × `0x2000` compressed bytes each). `fastfile.cpp`
  strips the master blocks to recover the stream; the hashes are only for the game's auth check.

## Tests

```sh
build/Release/iw4r_tests.exe                  # core logic, plus real game files if installed
build/Release/iw4r_structcheck.exe            # x64 asset layout guard (fails to build if a struct drifts)
build/Release/iw4radiant.exe --ui-test r.txt  # scripted mouse and keyboard through the real UI
```

## iw4r_zone (fastfile tool)

Reads and rebuilds unsigned (`IWffu100`) fastfiles, and reads signed (`IWffs100`) ones (the
retail MP maps) — `info` and `import` work on both; `rebuild` emits an unsigned zone.

```sh
build/Release/iw4r_zone.exe info <in.ff>            # header, prefix bytes, string/asset counts
build/Release/iw4r_zone.exe rebuild <in.ff> <out.ff>   # read then re-write, verifying round-trip
build/Release/iw4r_zone.exe compile <map.map> <out.ff>  # compile a .map to a zone (partial, see below)
```

### compile (map → zone, in progress)

`compile` reads a `.map` and emits an unsigned zone (`src/zone/zonebuild.cpp`) with the entity
lump (`MapEnts`) and the three simple world assets (`ComWorld`, `GameWorldMp`, `FxWorld`). The
container matches the conventions of a real loadable custom zone (`-2` asset headers, `-1` follow
pointers, `XFile.size` = inflated − 40, one null string-pool entry), and the output round-trips
through the reader. **It is not a playable map yet:** a map won't load in-game until collision
(`clipMap_t`) and rendering (`GfxWorld`) assets are added, and the `blockSize[]` split is still a
best-effort model pending verification against the running game. Those are the next milestones.

`rebuild` reads a zone, re-compresses it, and verifies the decompressed payload is byte-identical
before writing. It only writes `<out.ff>` and never touches the input or any game file. The
deflate level doesn't matter to the game, which only decompresses, so a rebuilt file can be a
different size while loading identically.
