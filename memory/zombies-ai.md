---
name: zombies-ai
description: "SP AI for a CoD-zombies mode: path node compiler (PathData format RE'd), actor_zombie aitype, maps\\_zombies (rounds, boarded windows); zm_test sample, working in-game"
metadata:
  node_type: memory
  type: project
  originSessionId: c635133b-e9c7-4023-9bad-d8499390d538
  modified: 2026-11-14T18:52:16.323Z
---

User goal (2026-11-14): zombie spawners + zombies tearing boards off windows like CoD Zombies.
User chose REAL SP AI (stock actors + compiled path nodes) over script-moved models.

**Path nodes (built, loader-sim + unit test testPathNodes pass, NOT yet in-game):**
node_pathnode entities -> GameWorldSp PathData (src/zone/pathnodes.{h,cpp}, zonebuild
buildGameWorldSp); stripped from the entity lump. Format decoded from stock trainer.ff (94 nodes)
+ iw4sp 2.0.14 IDA db (scratchpad ida/sp.exe.i64, imagebase 0x7ff7c2dc1000 = dump RVAs + base):
pathnode_t 168 (constant 72 written, rest zero; Path_Init 0x7ff7c2f698a0 clears runtime part,
recomputes forward, copies link count, SORTS links), links 12 (2D fDist, symmetric, stock max ~255),
basenodes RUNTIME block (game fills), chain arrays null, pathVis bit = hi + lo*n (lo<hi),
ceil(n(n-1)/8) bytes (Path_NodesVisible 0x7ff7c2f697f0), nodeTree kd on X/Y, child[0] < dist <=
child[1], leaves <= 4, children = back-refs into the array (0x7ff7c2f69630). g_path global
0x7ff7c3ab7088. Game node cap 8192. Compiler: drop to floor, link = hull sweep (14x14, step 18,
height 72) vs SOLID|monsterclip(0x20000) + floor probes every 16u; vis = ray at +60 vs SOLID.

**AI:** spawner classname actor_X -> game loads rawfile aitype/X.gsc and REQUIRES main/precache/
spawner (0x7ff7c2f55400, fatal if missing). common.ff has animscripts + generic_human.atr + ~1700
xanims (melee_1, traverse_window_quick, sprint1_loop present) but NO character bodies/AI weapons;
body = so_juggernauts_favela add-on (worldspawn weaponpack) body_complete_sp_juggernaut + m240.
New compiler feature: zone_source/scripts/<path>.gsc|.csv -> rawfile <path>.
Scripts: maps/zone_source/scripts/{aitype/zombie.gsc, character/zombie.gsc, maps/_zombies.gsc}.
_zombies: ignoreall + gun_remove melee rushers, zm_window script_origin (+target inside point),
zm_board script_models (tools/make_board.py), board tear via animscripted melee_1, climb
traverse_window_quick + forceteleport, hold Use inside to rebuild, points/round HUD (setvalue only,
no localized strings). Sample: tools/make_zm_test.py -> maps/zm_test.map/.gsc (play devmap so_w_zm_test).
VERIFIED IN-GAME 2026-11-14 (user): HUD, spawns, pathing to windows, board tearing, climb + melee all work, iprintlnbold plain text shows, nothing broken. Retail SP prints almost nothing to console (strings stripped) -> debug via live memory reads, not logs.
