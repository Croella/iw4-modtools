---
name: custom-sky
description: Custom skies from zone_source/skies/<sky>.png (equirect panorama) embedded inline as a cube; stock sky shader + IWI facts
metadata:
  type: project
---
Added 2026-11-13 (user asked for nighttime; no stock night sky exists in the MW2 IWDs: sky_*_ft images
there are all day/dusk; gulag = stormy dusk, arcadia = fiery dusk).
- Stock sky .iwi "_ft" files are whole CUBEMAPS (6 faces in one file, header 32 bytes; format byte at
  offset 8, dims at 10). Stock sky shader: texCUBE(s4, world-space direction, Z up), out = (rgb*a)^2.
- Compiler (zonebuild.cpp customSkyImage): if worldspawn "sky" = X and zone_source/skies/X.png|.tga
  exists, resample the equirect panorama (centre = +X, yaw grows left, top = up) into D3D cube faces
  (+X,-X,+Y,-Y,+Z,-Z; sqrt-encoded), inline A8R8G8B8, levelCount 1, flags 0x10002 (stock sky load def
  flags = cube|nomips), faces <= 512 (6 MB). Logged as "sky: embedded ... as a NxN cube". Loader sim ok;
  awaiting in-game test (face orientation unverified in game).
- tools/make_night_sky.py <out.png> [moon_pitch moon_yaw] [width]: procedural night panorama (stars,
  galactic band, airglow, moon at the sundirection). modeltest uses sky_night + moonlight keys; the
  day version is backed up in the session scratchpad only.
- Editor sky preview tint now divides by max(skyColor, 0.45) so night skies stay dark.
Related: [[lighting]], [[volumetric-lighting]].
