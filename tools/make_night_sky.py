"""Generates a night-sky panorama for the compiler's custom-sky support (zone_source/skies/<name>.png,
equirectangular: centre column = world +X, yaw grows to the left, top row = straight up).

Stars, a faint galactic band, horizon airglow and a moon at a given direction (match it to the
map's worldspawn sundirection so the moonlight comes from the moon).

usage: python tools/make_night_sky.py <out.png> [moon_pitch moon_yaw] [width]
  moon_pitch / moon_yaw: the worldspawn "sundirection" pitch and yaw (pitch negative = above the
  horizon, as in Radiant). Default -30 49.
"""
import math
import sys

import numpy as np
from PIL import Image

out = sys.argv[1]
pitch = float(sys.argv[2]) if len(sys.argv) > 3 else -30.0
yaw = float(sys.argv[3]) if len(sys.argv) > 3 else 49.0
W = int(sys.argv[4]) if len(sys.argv) > 4 else 4096
H = W // 2
rng = np.random.default_rng(1234)

# Direction of every pixel.
u = (np.arange(W) + 0.5) / W
v = (np.arange(H) + 0.5) / H
lon = (0.5 - u) * 2 * math.pi  # yaw
lat = (0.5 - v) * math.pi  # pitch up
LON, LAT = np.meshgrid(lon, lat)
X = np.cos(LAT) * np.cos(LON)
Y = np.cos(LAT) * np.sin(LON)
Z = np.sin(LAT)

# Linear radiance, then encoded to sRGB at the end.
img = np.zeros((H, W, 3))
elev = np.clip(Z, -1, 1)
# Sky gradient: deep navy zenith, a little lighter and greener-blue towards the horizon.
t = np.clip(elev, 0, 1) ** 0.45
zen = np.array([0.0015, 0.0025, 0.0075])
hor = np.array([0.010, 0.014, 0.024])
img += hor * (1 - t[..., None]) + zen * t[..., None]
# Airglow / distant light pollution: a thin warm band right at the horizon.
glow = np.exp(-np.clip(elev, 0, None) / 0.035) * (elev > -0.02)
img += glow[..., None] * np.array([0.020, 0.015, 0.012])
# Below the horizon: dark ground haze.
below = np.clip(-elev / 0.05, 0, 1)[..., None]
img = img * (1 - below) + np.array([0.004, 0.005, 0.007]) * below

# Galactic band: a great circle tilted against the horizon, with mottled brightness.
n = np.array([0.35, -0.55, 0.76])
n /= np.linalg.norm(n)
d = X * n[0] + Y * n[1] + Z * n[2]
band = np.exp(-(d / 0.16) ** 2)
small = rng.random((H // 32 + 2, W // 32 + 2))
mott = np.array(Image.fromarray((small * 255).astype(np.uint8)).resize((W, H), Image.BICUBIC)) / 255.0
small2 = rng.random((H // 8 + 2, W // 8 + 2))
mott2 = np.array(Image.fromarray((small2 * 255).astype(np.uint8)).resize((W, H), Image.BICUBIC)) / 255.0
band *= (0.45 + 0.8 * mott) * (0.7 + 0.5 * mott2)
img += (band * (elev > 0))[..., None] * np.array([0.012, 0.012, 0.016])

# Stars: uniform over the sphere, magnitudes skewed faint; splatted as small gaussians.
N = int(9000 * (W / 4096) ** 2)
vec = rng.normal(size=(N, 3))
vec /= np.linalg.norm(vec, axis=1)[:, None]
vec = vec[vec[:, 2] > -0.02]
mag = rng.random(len(vec)) ** 6  # mostly faint
temp = rng.random(len(vec))
for p, m, tc in zip(vec, mag, temp):
    lo_ = math.atan2(p[1], p[0])
    la_ = math.asin(max(-1, min(1, p[2])))
    px = (0.5 - lo_ / (2 * math.pi)) * W
    py = (0.5 - la_ / math.pi) * H
    bright = 0.05 + 2.2 * m
    # horizon extinction
    bright *= min(1.0, max(0.0, (p[2] + 0.02) / 0.12))
    col = np.array([1.0, 0.95, 0.85]) if tc < 0.25 else np.array([0.8, 0.88, 1.0]) if tc > 0.8 else np.array([1, 1, 1.0])
    sx = 0.55 / max(0.08, math.cos(la_))  # equirect stretch near the poles
    r = 2
    x0, y0 = int(px), int(py)
    for yy in range(y0 - r, y0 + r + 1):
        if yy < 0 or yy >= H:
            continue
        for xx in range(x0 - int(r * sx) - 1, x0 + int(r * sx) + 2):
            dx = (xx + 0.5 - px) / sx
            dy = yy + 0.5 - py
            g = math.exp(-(dx * dx + dy * dy) / (2 * 0.55 ** 2))
            if g < 0.01:
                continue
            img[yy, xx % W] += col * bright * g

# Moon: a disc with soft maria, plus a wide halo.
mp, my = math.radians(-pitch), math.radians(yaw)
m = np.array([math.cos(mp) * math.cos(my), math.cos(mp) * math.sin(my), math.sin(mp)])
cosang = np.clip(X * m[0] + Y * m[1] + Z * m[2], -1, 1)
ang = np.arccos(cosang)
R = math.radians(1.6)
disc = np.clip((R - ang) / math.radians(0.08), 0, 1)
# maria: low-frequency noise in a local frame on the disc
up = np.array([0, 0, 1.0])
e1 = np.cross(up, m)
e1 /= np.linalg.norm(e1)
e2 = np.cross(m, e1)
lx = (X * e1[0] + Y * e1[1] + Z * e1[2]) / R
ly = (X * e2[0] + Y * e2[1] + Z * e2[2]) / R
maria = np.ones_like(lx)
for cx, cy, rad, depth in ((-0.30, 0.25, 0.32, 0.30), (0.15, 0.35, 0.22, 0.25), (0.25, -0.05, 0.28, 0.28),
                           (-0.10, -0.30, 0.20, 0.18), (0.45, 0.30, 0.12, 0.15), (-0.45, -0.10, 0.15, 0.2)):
    maria -= depth * np.exp(-((lx - cx) ** 2 + (ly - cy) ** 2) / (rad * rad))
maria -= 0.12 * (lx * lx + ly * ly)  # slight limb darkening
img += disc[..., None] * maria[..., None] * np.array([0.82, 0.84, 0.88])
halo = np.exp(-ang / math.radians(6)) * 0.035 + np.exp(-ang / math.radians(25)) * 0.012
img += (halo * (elev > -0.05))[..., None] * np.array([0.75, 0.85, 1.0])

srgb = np.clip(img, 0, 1) ** (1 / 2.2)
Image.fromarray((srgb * 255 + 0.5).astype(np.uint8)).save(out)
print('wrote', out, W, 'x', H, 'moon pitch', pitch, 'yaw', yaw)
