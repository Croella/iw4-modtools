"""Generates maps/zm_test.map: a zombies sample for maps\\_zombies (SP).

A walled yard with a house in the middle. Each house wall has a boarded window (zm_board script
models, a zm_window script_origin outside, its inside target, and a clip_player brush in the
opening). actor_zombie spawners stand in the yard corners, node_pathnode entities cover the yard
and the house. Night lighting like modeltest. Writes zm_test.gsc next to it when there is none.

usage: python tools/make_zm_test.py [out.map]
"""
import os
import sys

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'maps', 'zm_test.map')
T = ' 256 256 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0'


def box(x0, y0, z0, x1, y1, z1, mat):
    f = [
        (x0, y0, z1, x0, y1, z1, x1, y0, z1),
        (x0, y0, z0, x1, y0, z0, x0, y1, z0),
        (x0, y0, z0, x0, y0, z1, x1, y0, z0),
        (x0, y1, z0, x1, y1, z0, x0, y1, z1),
        (x0, y0, z0, x0, y1, z0, x0, y0, z1),
        (x1, y0, z0, x1, y0, z1, x1, y1, z0),
    ]
    return '{\n' + ''.join(' ( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s%s\n' % (p + (mat, T)) for p in f) + '}\n'


YARD = 1024        # yard half size
SKYH = 640
HOUSE = 256        # house inner half size
WALL = 16
WALLH = 160
WIN_W = 32         # window half width
SILL, TOP = 40, 104
brushes = []
# ground and sky box
brushes.append(box(-YARD, -YARD, -16, YARD, YARD, 0, 'ch_asphalt01'))
S = YARD + 16
brushes.append(box(-S, -S, SKYH, S, S, SKYH + 16, 'sky'))
brushes.append(box(-S, -S, -16, -YARD, S, SKYH, 'sky'))
brushes.append(box(YARD, -S, -16, S, S, SKYH, 'sky'))
brushes.append(box(-YARD, -S, -16, YARD, -YARD, SKYH, 'sky'))
brushes.append(box(-YARD, YARD, -16, YARD, S, SKYH, 'sky'))
# house walls with a window in the middle of each (x walls run along Y, y walls along X)
H, O = HOUSE, HOUSE + WALL
mat = 'airport_wall_interior01'
for side in (-1, 1):
    # walls at x = +-(H..O), spanning y -O..O
    xa, xb = (H, O) if side > 0 else (-O, -H)
    brushes.append(box(xa, -O, 0, xb, -WIN_W, WALLH, mat))
    brushes.append(box(xa, WIN_W, 0, xb, O, WALLH, mat))
    brushes.append(box(xa, -WIN_W, 0, xb, WIN_W, SILL, mat))
    brushes.append(box(xa, -WIN_W, TOP, xb, WIN_W, WALLH, mat))
    brushes.append(box(xa, -WIN_W, SILL, xb, WIN_W, TOP, 'clip_player'))
    # walls at y = +-(H..O), spanning x -H..H (corners belong to the x walls)
    ya, yb = (H, O) if side > 0 else (-O, -H)
    brushes.append(box(-H, ya, 0, -WIN_W, yb, WALLH, mat))
    brushes.append(box(WIN_W, ya, 0, H, yb, WALLH, mat))
    brushes.append(box(-WIN_W, ya, 0, WIN_W, yb, SILL, mat))
    brushes.append(box(-WIN_W, ya, TOP, WIN_W, yb, WALLH, mat))
    brushes.append(box(-WIN_W, ya, SILL, WIN_W, yb, TOP, 'clip_player'))

ents = []


def ent(**kv):
    ents.append('{\n' + ''.join('"%s" "%s"\n' % (k, v) for k, v in kv.items()) + '}\n')


ent(classname='info_player_start', origin='0 0 16', angles='0 0 0')
# windows: (outside point, inward yaw, board axis yaw, name)
windows = [((O + 40, 0), 180, 0, 'e'), ((-O - 40, 0), 0, 0, 'w'), ((0, O + 40), 270, 90, 'n'), ((0, -O - 40), 90, 90, 's')]
for (wx, wy), yaw, byaw, nm in windows:
    ent(classname='script_origin', targetname='zm_window', origin='%g %g 8' % (wx, wy), angles='0 %d 0' % yaw, target='zm_in_' + nm)
    ix, iy = (wx * (H - 48) / abs(wx), 0) if wx else (0, wy * (H - 48) / abs(wy))
    ent(classname='script_origin', targetname='zm_in_' + nm, origin='%g %g 8' % (ix, iy), angles='0 %d 0' % yaw)
    # boards on the outside face of the wall
    bx, by = (wx / abs(wx) * (O + 2), 0) if wx else (0, wy / abs(wy) * (O + 2))
    for k, (z, roll) in enumerate(((50, 6), (64, -4), (78, 5), (92, -7))):
        ent(classname='script_model', model='zm_board', targetname='zm_board', origin='%g %g %g' % (bx, by, z),
            angles='0 %d %d' % (byaw, roll))
for x, y in ((-800, -800), (800, -800), (-800, 800), (800, 800)):
    ent(classname='actor_zombie', origin='%d %d 8' % (x, y), spawnflags='1', targetname='zm_spawner', angles='0 0 0')
# path nodes: yard grid outside the house, house grid inside, plus the window points
nodes = []
for x in range(-960, 961, 96):
    for y in range(-960, 961, 96):
        if max(abs(x), abs(y)) < O + 40:
            continue
        nodes.append((x, y))
for x in range(-H + 48, H - 47, 80):
    for y in range(-H + 48, H - 47, 80):
        nodes.append((x, y))
for (wx, wy), _, _, _ in windows:
    nodes.append((wx, wy))
    nodes.append(((wx * (H - 48) / abs(wx)) if wx else 0, (wy * (H - 48) / abs(wy)) if wy else 0))
for x, y in nodes:
    ent(classname='node_pathnode', origin='%g %g 16' % (x, y))
# effects (zone_source/fx, BO1 .efx files): burning barrels by the spawners, a smoke plume over
# the yard, low fog drifting around the house, falling embers
for x, y in ((-700, -700), (700, -700), (-700, 700), (700, 700)):
    ent(classname='fx_origin', origin='%d %d 0' % (x, y), fx='env/fire/fx_fire_barrel_small')
ent(classname='fx_origin', origin='600 0 0', fx='env/smoke/fx_smk_plume_md_blk')
for x, y in ((0, -600), (0, 600), (-600, 0)):
    ent(classname='fx_origin', origin='%d %d 0' % (x, y), fx='env/smoke/fx_fog_low_sm')
ent(classname='fx_origin', origin='0 0 400', fx='env/fire/fx_embers_falling_md')

world = {
    'classname': 'worldspawn', 'weaponpack': 'so_juggernauts_favela',
    'sky': 'sky_night', 'sundirection': '-30 49 0', 'sunlight': '0.35', 'suncolor': '0.62 0.72 1',
    'diffusefraction': '0.3', 'sundiffusecolor': '0.22 0.3 0.55', 'ambient': '0.04',
    'volumetric': '1', 'volumetric_intensity': '0.45', 'volumetric_color': '0.62 0.72 1', 'volumetric_threshold': '0.12',
    'volumetric_glow': '0.6', 'volumetric_glowradius': '0.05', 'volumetric_fog': '1', 'volumetric_fog_density': '0.0007',
    'volumetric_fog_color': '0.035 0.045 0.075', 'volumetric_fog_sun': '0.25', 'volumetric_fog_max': '0.8',
}
s = 'iwmap 4\n"000_Global" flags  active\n"The Map" flags  \n{\n'
s += ''.join('"%s" "%s"\n' % kv for kv in world.items())
for i, b in enumerate(brushes):
    s += '// brush %d\n' % i + b
s += '}\n' + ''.join(ents)
out = os.path.abspath(out)
open(out, 'w').write(s)
print('wrote', out, len(brushes), 'brushes', len(ents), 'entities', len(nodes), 'path nodes')
gsc = os.path.splitext(out)[0] + '.gsc'
if not os.path.exists(gsc):
    open(gsc, 'w').write('''// zm_test: maps\\_zombies sample (zone_source/scripts/maps/_zombies.gsc)
main()
{
	precachemodel("viewhands_op_force");
	precacheitem("ak74uwu");
	maps\\_load::main();
	maps\\_zombies::main();
	level.player thread loadout();
}

loadout()
{
	wait 1;  // after the stock loadout
	self takeallweapons();
	self setviewmodel("viewhands_op_force");
	self giveweapon("ak74uwu");
	self givemaxammo("ak74uwu");
	self switchtoweapon("ak74uwu");
}
''')
    print('wrote', gsc)
