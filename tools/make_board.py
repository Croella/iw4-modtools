"""Writes a box-shaped xmodel_export (one bone, tag_origin): the zombies window board.

usage: python tools/make_board.py <out.xmodel_export> [name] [sx sy sz] [texture]
  sx sy sz: box size (default 2 x 56 x 7: thin along X, long along Y, centred on the origin)
  texture:  png in zone_source/images (default crate_wood.png)
"""
import sys

out = sys.argv[1]
name = sys.argv[2] if len(sys.argv) > 2 else 'zm_board'
sx, sy, sz = (float(v) for v in sys.argv[3:6]) if len(sys.argv) > 5 else (2.0, 56.0, 7.0)
tex = sys.argv[6] if len(sys.argv) > 6 else 'crate_wood.png'
hx, hy, hz = sx / 2, sy / 2, sz / 2
corners = [(x, y, z) for z in (-hz, hz) for y in (-hy, hy) for x in (-hx, hx)]
idx = {c: i for i, c in enumerate(corners)}
# each face: normal + its 4 corners counter-clockwise seen from outside (cross(b-a, c-a) = normal)
faces = []
for axis in range(3):
    for sign in (-1, 1):
        n = [0.0, 0.0, 0.0]
        n[axis] = float(sign)
        u, v = [(1, 2), (2, 0), (0, 1)][axis]
        h = (hx, hy, hz)
        quad = []
        for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            p = [0.0, 0.0, 0.0]
            p[axis] = sign * h[axis]
            p[u] = a * h[u]
            p[v] = b * h[v]
            quad.append(tuple(p))
        if sign < 0:
            quad.reverse()
        # uv: wood grain along the long side (Y), 1 texture repeat per 56 units
        uvs = []
        for p in quad:
            along = p[1] if axis != 1 else p[0]
            across = p[2] if axis != 2 else p[0]
            uvs.append(((along + hy) / 56.0, (across + hz) / 56.0 * 8))
        faces.append((n, quad, uvs))

L = ['// iw4-radiant zombies window board (tools/make_board.py)', 'MODEL', 'VERSION 6', '', 'NUMBONES 1',
     'BONE 0 -1 "tag_origin"', '', 'BONE 0', 'OFFSET 0.000000, 0.000000, 0.000000', 'SCALE 1.000000, 1.000000, 1.000000',
     'X 1.000000, 0.000000, 0.000000', 'Y 0.000000, 1.000000, 0.000000', 'Z 0.000000, 0.000000, 1.000000', '',
     'NUMVERTS %d' % len(corners)]
for i, c in enumerate(corners):
    L += ['VERT %d' % i, 'OFFSET %f, %f, %f' % c, 'BONES 1', 'BONE 0 1.000000', '']
L.append('NUMFACES %d' % (2 * len(faces)))
for n, quad, uvs in faces:
    for tri in ((0, 1, 2), (0, 2, 3)):
        L.append('TRI 0 0 0 0')
        for k in tri:
            L += ['VERT %d' % idx[quad[k]], 'NORMAL %f %f %f' % tuple(n), 'COLOR 1.000000 1.000000 1.000000 1.000000',
                  'UV 1 %f %f' % uvs[k]]
L += ['', 'NUMOBJECTS 1', 'OBJECT 0 "%s"' % name, '', 'NUMMATERIALS 1',
      'MATERIAL 0 "%s" "Lambert" "color:c:/art/%s"' % (name, tex), 'COLOR 0.000000 0.000000 0.000000 1.000000', '']
open(out, 'w').write('\n'.join(L))
print('wrote', out)
