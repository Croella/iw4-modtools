---
name: signed-zone-format
description: How IW4 signed (IWffs100) fastfiles are decoded so the editor can read the retail MP maps
metadata:
  node_type: memory
  type: project
  originSessionId: 5594d009-4322-40bf-84d3-5c0c822a3f25
  modified: 2026-11-11T17:18:07.831Z
---

Signed IW4 zones (`IWffs100`, the retail multiplayer `mp_*.ff` maps) are NOT encrypted — same
zlib payload as unsigned zones, just with authentication data interleaved. SOLVED 2026-11-11;
iw4-radiant reads them (editor imports MP maps, material scanner reads them).

**Format** (exact structure from `DB_AuthHeader`/`DB_MasterBlock` in the Xbox IDA header
`C:\Users\Lurkz\Desktop\iw4mp pdb\2-iw4mp.exe.h`, confirmed byte-for-byte against the files):
- File header: `IWff0100` + u32 version(276) + 9 prefix bytes, then `IWffs100` subheader at 0x15.
- `DB_AuthHeader` at 0x15: magic(8) + reserved(4) + subheaderHash(32) + RSA-2048 signature(256) +
  subheader{ fastfileName[32] + reserved(4) + masterBlockHashes[244] }. (fastfileName at 0x141
  literally holds e.g. "mp_highrise" — a good landmark to confirm offsets.)
- Then a `DB_MasterBlock` (256 SHA-256 chunk hashes = 0x2000). Compressed data starts at **0x4015**.
- The zlib stream is interrupted every **0x200000** bytes by a fresh **0x2000**-byte master block
  (256 chunks × 0x2000 compressed bytes each). **Strip the master blocks → one continuous zlib
  stream → inflate.** The hashes only matter to the game's integrity check; the editor ignores them.
- 0x4015 / 0x200000 / 0x2000 are constant across every signed zone in the install (all `mp_*`,
  `common_mp`, `patch_mp`) — expected, since the auth header is fixed-size.

**Why:** the editor needs to read the retail MP maps (to import geometry and harvest materials).
This is editor-side decode only — nothing to do with making the *game* load custom zones.

**How to apply:** implemented in `src/fastfile.cpp` `stripSignedStream()`, wired into
`inflateFastfile`/`readFastfile` (sets `Fastfile::signedZone`). Importer + material scanner handle
signed zones via the normal stock-zone path (same as unsigned SP maps like boneyard). `iw4r_zone
info`/`import` work on signed; `rebuild` reads signed and emits an unsigned copy of the payload.
Test imports `mp_highrise.ff` (24239 brushes / 9526 surfaces). See [[x64-zone-format]], [[zone-linker]].
