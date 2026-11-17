---
name: x64-zone-format
description: "Findings about the x64 IW4 fastfile format that aren't obvious from the code, plus next unknowns"
metadata:
  node_type: memory
  type: project
  originSessionId: 4a87fe3d-0fa1-4aac-8adf-aa87c4c6e1de
  modified: 2026-11-11T17:18:12.701Z
---

The install's zones are version 276 but use 64-bit pointers on disk, so 32-bit community docs and OpenAssetTools layouts don't apply. Verified 2026-11-04: all 2913 asset rows of boneyard.ff parse as `{type, pad, ptr8}`.

Open questions, in rough order of importance for the compiler:
- Signed MP zones (`IWffs100`): SOLVED 2026-11-11 — not encrypted, just SHA-256 auth blocks interleaved into the zlib stream. The editor now reads them; see [[signed-zone-format]]. (Whether the x64 build enforces signatures on *custom* zones is a separate, still-open game-side question.)
- XAssetType numbering differs from the Xbox header (types above 0x28 appear). Recover it from the x64 binary.
- World asset layouts (clipMap_t, GfxWorld, etc.) in x64 are unmapped.
- Pointer back-references like 0x30007f8d decode as block=(p-1)>>28, offset=(p-1)&0x0FFFFFFF (resolved 2026-11-09, see [[zone-linker]]).

**Why:** the editor uses a pattern scan for materials, which is a stopgap. The compiler needs exact layouts.

**Unsigned header prefix (decoded 2026-11-04):** an IWffu100 file is magic[8] + u32 version(276) + 9 opaque bytes + one zlib stream (offset 21). The 9 bytes: byte0 = flag (01 in every stock zone, 00 in the client's own serenity_controller_common.ff), then a build stamp (~0x01dd3bXX across stock files) and a per-file value. The client's loader does not require meaningful values there - serenity zeroes the stamp/tail and loads fine - so a from-scratch build can use a near-zero prefix. iw4-radiant now reads and writes this container (Fastfile / readFastfile / writeFastfile in src/fastfile.cpp), with an `iw4r_zone` CLI (info/rebuild).

**Milestone done:** zone-writer round-trip verified read->write->read byte-identical on serenity_controller_common.ff (10891-byte payload) and stock boneyard.ff (245 MB payload). Deflate level is free since the game only inflates. Still open: confirm the client loads a rebuilt file in-game, and signed IWffs100 zones remain unreadable.

**How to apply:** next step for the compiler is dumping a real map's world assets from the x64 build, and proving a custom zone loads, before writing BSP code. See [[project-direction]].
