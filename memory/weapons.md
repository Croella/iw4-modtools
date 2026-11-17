---
name: weapons
description: "Custom weapons (loads in-game, layout validated) compiled from zone_source/weapons (IW4 weapon files) + RE'd x64 WeaponCompleteDef/WeaponDef layout; older weapon-pack shortcut"
metadata:
  node_type: memory
  type: project
  originSessionId: c635133b-e9c7-4023-9bad-d8499390d538
  modified: 2026-11-14T21:00:05.397Z
---

**Real weapon embedding (M3; LOADS IN-GAME in SP 2026-11-12, layout validated live):**
zone_source/weapons/<name> (IW4 "WEAPONFILE\key\value" text, no extension; asset = file name,
lowercase) -> WeaponCompleteDef (160) + WeaponDef (2192). Code: src/zone/weapondef.{h,cpp}
(field table key->offset/kind, enum tables, parser), zonebuild buildWeapon (writes in loader
order), zonesim weaponPtr (type 28), live::dumpWeapons + CLI `iw4r_zone liveweapon [name] --sp`
(decodes a running game's weapon through the same table: use it on stock weapons to validate).
Layout = 32-bit IW4 structs widened; every pointer offset confirmed from the x64 loaders
(iw4mp Load_WeaponCompleteDef 0x120810, Load_WeaponDef 0x120c30; type switch 0x122e30 case 28 ->
0x120b70) and scalar gaps sum exactly. UNVERIFIED: the order of the 47 sounds (struct order
assumed: ...nightVisionRemove, altSwitch, raise, firstRaise, putaway, scan; the file lists
altSwitch after firstRaise) and the 46 bools at 2140.. -> check with liveweapon on stock guns.
Sounds = name slot (-1) + string, resolved by name at load (0x12d6d0 type 11); FX (struct 40,
0x118f30), XModels, Materials written as ','-name references; anims = name strings (37 slots,
written to complete+32 and def+24/+32); tracer/physCollmap/accuracy graphs null.
Weapon refs resolve to zone_source assets ignoring t5_/weapon_/viewmodel_/_lodN tokens; missing
hand model -> first viewhands* model; missing anims left empty (warned). Gun/world models get the
weapon's hideTags dropped at compile (bones + children) + BO1 decal materials, gun/hand models
zero trans. Anims: parents from any model with the bone; >256 frames resampled to 256.
User's test: AK-74u (BO1 export: t5_weapon_ak74u_viewmodel/LOD0..3, 13 xanims, no fire/ADS
anims), weapon file maps/zone_source/weapons/ak74uwu (I set its hideTags to the attachment tags;
original saved in scratchpad ak74uwu.orig). maps/modeltest.gsc (user-edited) precaches + gives it.
Sounds/FX are stock MW2 names (weap_ak47_*, muzzleflashes/ak47_flash_view) -> silent/default
unless a loaded zone has them. Next: in-game test, liveweapon validation, notetracks
(sndnt#... -> XAnimParts notify), extra LODs, ADS without ads anims.

**Older shortcut (weapon packs):** worldspawn `weaponpack` = stock SO add-on zone (e.g.
so_escape_airport has the rpg/ak?), level so_w_<map>; see zonebuild weaponPacks().
Add-on level = "so_" + name containing '_' (iw4sp 0x7ff7c301e650); game looks up type 42
"maps/<level>.mapents" (0x7ff7c2fb01c0). addon_map_ents pool size is 1 and every stock pack has
its own -> our zone must NOT emit one ("exceeded limit of 1 'addon_map_ents'", 2026-11-14).
deployWeaponPack now rewrites the pack: retargetAddonMapEnts renames its lump in the same bytes
(name + blanked entity string, triggers kept) and recompresses.
Loading a 2nd stock MAP zone is impossible (singleton pools; override is fatal iw4sp+0xf00f0).
Crash 1 (2026-11-11, iw4sp+0x85cdb, ammo/clip/sharedAmmoCap name table fn 0x85be0): engine reads
WeaponDef string fields unchecked. Convention now = the game's weapon-file loader's: every string
field non-null ("" when empty), all 37 anim slots non-null, hideTags/notetrack maps/bounce/
locationDamage arrays always present; asset pointers (model/sound/fx/material) null when unused.
Crash dumps: %LOCALAPPDATA%\CrashDumps\iw4sp.exe.<pid>.dmp (stacks only, no heap) + WER
ReportArchive Report.wer (UTF-16, "Exception Offset" = RVA).
Crash 2 (2026-11-12, iw4sp+0x29d225, notify lookup 0x29c970 reads XAnimParts.notify (+0x78) with
no count check): every compiled anim now gets notetracks (export NOTETRACKS, lowercased, time =
frame/(frames-1)) plus "end" at 1.0 like stock anims. Disassemble SP crashes with scratchpad
fn.py (capstone; run from outside scratchpad, a dis.py there shadowed stdlib -> renamed cdis.py).
VALIDATED 2026-11-12 with liveweapon on running iw4sp (defaultweapon, fraggrenade, ak74uwu): all
fields decode sensibly (locationDamage table, tracer defaulttracer, accuracy graphs default.accu,
sounds nightvision*_plr_default / weap_raise / weap_putaway in the assumed slots, grenade bools
rotate/timedDetonation/cookOffHold). altSwitch vs firstRaise sound order still unproven (unused).
SP weapon pool slots are 2192 bytes (+8 head), MP 160: liveimport tries both.
