"""Generates the zone_source animation demo: a 4-bone skinned column (demo_tail.xmodel_export)
and a looping sway (demo_tail_loop.xanim_export), written next to maps/modeltest.map.

    python tools/demo_tail.py
"""
import math
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'maps', 'zone_source')
SEG = 24.0    # bone spacing
HALF = 8.0    # column half width
NSEG = 4
BONES = ['tag_origin'] + ['j_tail%d' % (i + 1) for i in range(NSEG)]
PARENTS = [-1, 0] + list(range(1, NSEG))
BIND = [(0.0, 0.0, 0.0)] + [(0.0, 0.0, SEG * i) for i in range(NSEG)]


def f(v):
    return '%.6f' % v


def write_model():
    rings = NSEG + 1
    corners = [(-HALF, -HALF), (HALF, -HALF), (HALF, HALF), (-HALF, HALF)]
    verts = []  # (pos, [(bone, weight)])
    for r in range(rings):
        if r == 0:
            w = [(1, 1.0)]
        elif r == rings - 1:
            w = [(NSEG, 1.0)]
        else:
            w = [(r, 0.5), (r + 1, 0.5)]
        for (x, y) in corners:
            verts.append(((x, y, SEG * r), w))
    faces = []  # [(vert, normal, uv)] * 3

    def quad(a, b, c, d, n, u0, u1, v0, v1):
        faces.append([(a, n, (u0, v0)), (b, n, (u1, v0)), (c, n, (u1, v1))])
        faces.append([(a, n, (u0, v0)), (c, n, (u1, v1)), (d, n, (u0, v1))])

    side_normals = [(0, -1, 0), (1, 0, 0), (0, 1, 0), (-1, 0, 0)]
    for r in range(rings - 1):
        for s in range(4):
            a, b = r * 4 + s, r * 4 + (s + 1) % 4
            quad(a, b, b + 4, a + 4, side_normals[s], 0, 1, r / NSEG, (r + 1) / NSEG)
    top = (rings - 1) * 4
    quad(0, 3, 2, 1, (0, 0, -1), 0, 1, 0, 1)
    quad(top, top + 1, top + 2, top + 3, (0, 0, 1), 0, 1, 0, 1)

    out = ['// iw4-radiant animation demo: 4-bone skinned column (tools/demo_tail.py)', 'MODEL', 'VERSION 6', '']
    out.append('NUMBONES %d' % len(BONES))
    for i, name in enumerate(BONES):
        out.append('BONE %d %d "%s"' % (i, PARENTS[i], name))
    out.append('')
    for i, o in enumerate(BIND):
        out += ['BONE %d' % i, 'OFFSET %s, %s, %s' % tuple(map(f, o)), 'SCALE 1.000000, 1.000000, 1.000000',
                'X 1.000000, 0.000000, 0.000000', 'Y 0.000000, 1.000000, 0.000000', 'Z 0.000000, 0.000000, 1.000000', '']
    out.append('NUMVERTS %d' % len(verts))
    for i, (p, w) in enumerate(verts):
        out += ['VERT %d' % i, 'OFFSET %s, %s, %s' % tuple(map(f, p)), 'BONES %d' % len(w)]
        out += ['BONE %d %s' % (b, f(wt)) for b, wt in w]
        out.append('')
    out.append('NUMFACES %d' % len(faces))
    for tri in faces:
        out.append('TRI 0 0 0 0')
        for v, n, uv in tri:
            out += ['VERT %d' % v, 'NORMAL %s %s %s' % tuple(map(f, n)), 'COLOR 1.000000 1.000000 1.000000 1.000000',
                    'UV 1 %s %s' % (f(uv[0]), f(uv[1]))]
    out += ['', 'NUMOBJECTS 1', 'OBJECT 0 "tail"', '', 'NUMMATERIALS 1',
            'MATERIAL 0 "tail" "Lambert" "color:c:/art/crate_wood.png"', 'COLOR 0.000000 0.000000 0.000000 1.000000', '']
    path = os.path.join(ROOT, 'xmodels', 'demo_tail.xmodel_export')
    open(path, 'w', newline='\n').write('\n'.join(out))
    print('wrote', os.path.normpath(path))


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def qrot(q, v):
    x, y, z, w = qmul(qmul(q, (v[0], v[1], v[2], 0.0)), (-q[0], -q[1], -q[2], q[3]))
    return (x, y, z)


def write_anim():
    frames = 61  # 2 seconds at 30 fps; last frame == first so it loops cleanly
    out = ['// iw4-radiant animation demo: sway loop for demo_tail (tools/demo_tail.py)', 'ANIMATION', 'VERSION 3', '',
           'NUMPARTS %d' % len(BONES)]
    out += ['PART %d "%s"' % (i, n) for i, n in enumerate(BONES)]
    out += ['', 'FRAMERATE 30', 'NUMFRAMES %d' % frames]
    for fr in range(frames):
        t = fr / (frames - 1)
        out.append('FRAME %d' % fr)
        q, p = (0.0, 0.0, 0.0, 1.0), (0.0, 0.0, 0.0)
        for i in range(len(BONES)):
            if i == 0:
                q, p = (0.0, 0.0, 0.0, 1.0), (0.0, 0.0, 0.0)
            else:
                # each segment bends about X (side to side) and Y a bit, phase-shifted up the chain
                ang = math.radians(14.0) * math.sin(2 * math.pi * t - 0.7 * (i - 1))
                angy = math.radians(6.0) * math.sin(4 * math.pi * t - 0.5 * (i - 1))
                local = qmul((math.sin(ang / 2), 0.0, 0.0, math.cos(ang / 2)), (0.0, math.sin(angy / 2), 0.0, math.cos(angy / 2)))
                local_t = (0.0, 0.0, 0.0 if i == 1 else SEG)
                off = qrot(q, local_t)
                p = (p[0] + off[0], p[1] + off[1], p[2] + off[2])
                q = qmul(q, local)
            ax = [qrot(q, (1, 0, 0)), qrot(q, (0, 1, 0)), qrot(q, (0, 0, 1))]
            out += ['PART %d' % i, 'OFFSET %s, %s, %s' % tuple(map(f, p))]
            out += ['%s %s, %s, %s' % (k, f(a[0]), f(a[1]), f(a[2])) for k, a in zip('XYZ', ax)]
        out.append('')
    out += ['NOTETRACKS', '']
    path = os.path.join(ROOT, 'xanims', 'demo_tail_loop.xanim_export')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, 'w', newline='\n').write('\n'.join(out))
    print('wrote', os.path.normpath(path))


write_model()
write_anim()
