---
name: map-folder-layout
description: "Repo maps/ holds (zm_test, geotest) one folder per map: maps/<map>/<map>.map + <map>.gsc + zone_source/ + compiled .ff; no shared maps/zone_source any more"
metadata:
  node_type: memory
  type: project
  originSessionId: c635133b-e9c7-4023-9bad-d8499390d538
  modified: 2026-11-15T23:30:42.626Z
---

User asked 2026-11-15: "make it so maps/ is the dir for folders containing maps/<mapname>/<zone_source>...
maps/<mapname>/<mapname>.gsc". They moved zm_test themselves (maps/zm_test/zm_test.map +
zone_source/) and DELETED estate/modeltest/mp_cliffhanger (I restored them into per-map folders; they
deleted those folders again -> only zm_test is wanted; old files remain in git HEAD).
2026-11-16 the user asked to keep maps/geotest too (world-geometry test: curve + mesh patch, door
script_brushmodel, crates, stock barrel; see [[world-geometry]]).

Code already resolved zone_source/script/outputs relative to the .map (zoneSourcePath, mapScriptPath,
compile default <map>.ff beside it). Added in app.cpp: mapsRoot() (Open dialog starts in maps/),
mapFolderPath() (saving maps/foo.map -> maps/foo/foo.map), Save As copies zone_source + <map>.gsc to
the new folder. Tests read maps/zm_test/zone_source; tools/make_zm_test.py writes
maps/zm_test/zm_test.map; tools/demo_tail.py [map] writes maps/<map>/zone_source (default zm_test).

**How to apply:** older memories saying "maps/zone_source" or "maps/modeltest.map" mean the old
shared layout; today it's maps/zm_test/zone_source. Related: [[zone-source]], [[effects]].
