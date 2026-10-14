#!/usr/bin/env python3
# ps5-spotify - Renders the Spot5 icon as glossy 3D shapes (height fields).
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: render_icon.py <concept a|b|c> <out.png> [size] [--mark]
# --mark leaves the background transparent (for use inside the app).
import sys
import numpy as np
from PIL import Image, ImageFilter

N = 2048                       # render size; downsampled for anti-aliasing
y, x = np.mgrid[0:N, 0:N].astype(np.float32) / N   # 0..1, y down

GREEN = np.array([0x1e, 0xd7, 0x60]) / 255
DARK = np.array([0x12, 0x14, 0x13]) / 255
WHITE = np.array([0.96, 0.98, 0.96])


def arc_sdf(cx, cy, r, t, a0, a1):
    dx, dy = x - cx, y - cy
    a = np.arctan2(dy, dx)
    inside = (a >= a0) & (a <= a1)
    d_ring = np.abs(np.hypot(dx, dy) - r) - t / 2
    caps = [np.hypot(x - (cx + r * np.cos(e)), y - (cy + r * np.sin(e))) - t / 2 for e in (a0, a1)]
    return np.where(inside, d_ring, np.minimum(*caps)), t / 2


def disc_sdf(cx, cy, r):
    return np.hypot(x - cx, y - cy) - r, r


def tilted_arc_sdf(cx, cy, half, sag, t, tilt):
    # A shallow bow (chord 2*half, rising sag) turned by tilt: as an arc of a big circle.
    R = (half * half + sag * sag) / (2 * sag)
    ha = np.arcsin(half / R)
    c, s = np.cos(tilt), np.sin(tilt)
    ox, oy = cx - R * s * -1 * 0, cy + R  # centre below the chord's middle (before tilt)
    # rotate coordinates about the chord's middle
    px = (x - cx) * c + (y - cy) * s + cx
    py = -(x - cx) * s + (y - cy) * c + cy
    dx, dy = px - cx, py - (cy + R)
    a = np.arctan2(dy, dx) + np.pi / 2
    inside = np.abs(a) <= ha
    d_ring = np.abs(np.hypot(dx, dy) - R) - t / 2
    ends = [(cx + R * np.sin(e), cy + R - R * np.cos(e)) for e in (-ha, ha)]
    caps = [np.hypot(px - ex, py - ey) - t / 2 for ex, ey in ends]
    return np.where(inside, d_ring, np.minimum(*caps)), t / 2


def height(sdf, bevel, dome=False):
    s = np.clip(-sdf, 0, None)
    if dome:  # a sphere-like cap over the whole shape
        return np.sqrt(np.clip(bevel ** 2 - (bevel - np.minimum(s, bevel)) ** 2, 0, None))
    b = min(bevel, 0.035)
    return np.sqrt(np.clip(b ** 2 - (b - np.minimum(s, b)) ** 2, 0, None)) + np.clip(s - b, 0, None) * 0.0


def shade(h, base, gloss=1.0, scale=1.0):
    gy, gx = np.gradient(h * N * scale)
    n = np.dstack([-gx, -gy, np.ones_like(h)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    L = np.array([-0.45, -0.65, 0.62]); L /= np.linalg.norm(L)
    V = np.array([0, 0, 1.0])
    H = L + V; H /= np.linalg.norm(H)
    diff = np.clip(n @ L, 0, 1)
    spec = np.clip(n @ H, 0, 1) ** 60 * gloss
    # environment: a bright sky above, dark floor below, seen in the reflection
    r = 2 * n[..., 2:3] * n - V
    sky = np.clip(-r[..., 1] * 0.5 + 0.5, 0, 1) ** 2
    fres = (1 - np.clip(n[..., 2], 0, 1)) ** 3
    col = base * (0.30 + 0.75 * diff)[..., None]
    col += (0.18 * sky * gloss + 0.35 * fres * gloss)[..., None] * np.array([1, 1, 1])
    col += spec[..., None] * np.array([1, 1, 1])
    # deep occlusion at the foot of each shape
    col *= (0.75 + 0.25 * np.clip(h / (h.max() + 1e-6) * 3, 0, 1))[..., None]
    return np.clip(col, 0, 1)


def mask(sdf):
    return np.clip(0.5 - sdf * N, 0, 1)


def shadow(m, dx=0.012, dy=0.03, blur=0.025, strength=0.55):
    img = Image.fromarray((m * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(blur * N))
    a = np.asarray(img).astype(np.float32) / 255
    a = np.roll(np.roll(a, int(dy * N), 0), int(dx * N), 1)
    return a * strength


def background(kind):
    if kind == "dark":
        g = np.hypot(x - 0.35, y - 0.65)
        col = DARK + (GREEN * 0.22) * np.clip(1 - g / 0.75, 0, 1)[..., None] ** 2
        col = col * (1.05 - 0.25 * y)[..., None]
    else:
        col = GREEN * (1.12 - 0.30 * y - 0.10 * x)[..., None]
        g = np.hypot(x - 0.25, y - 0.15)
        col = col + 0.10 * np.clip(1 - g / 0.6, 0, 1)[..., None]
    return np.clip(col, 0, 1)


def concept(name):
    """Returns (background kind, [(sdf, bevel, colour, dome)])"""
    if name in ("a", "b"):
        cx, cy = 0.29, 0.71
        col = GREEN * 1.05 if name == "a" else WHITE
        shapes = [disc_sdf(cx, cy, 0.095) + (col, True)]
        for i, r in enumerate((0.22, 0.33, 0.44)):
            sdf, b = arc_sdf(cx, cy, r, 0.065, -np.pi * 0.5 + 0.05, -0.05)
            shapes.append((sdf, b, col, False))
        return ("dark" if name == "a" else "green"), shapes
    # c: three tilted bows on green, dot of the "5" style at the right end
    shapes = []
    for (cy, half, sag, t) in ((0.39, 0.30, 0.085, 0.080), (0.55, 0.245, 0.065, 0.064), (0.69, 0.19, 0.048, 0.052)):
        sdf, b = tilted_arc_sdf(0.5, cy, half, sag, t, -0.12)
        shapes.append((sdf, b, WHITE, False))
    return "green", shapes


def render(name, mark_only):
    kind, shapes = concept(name)
    col = np.zeros((N, N, 3)) if mark_only else background(kind)
    alpha = np.zeros((N, N)) if mark_only else np.ones((N, N))
    union = np.zeros((N, N))
    for sdf, bevel, _, _ in shapes:
        union = np.maximum(union, mask(sdf))
    sh = shadow(union)
    if not mark_only:
        col = col * (1 - sh)[..., None]
    else:
        alpha = np.maximum(alpha, sh * 0.8)
    for sdf, bevel, base, dome in shapes:
        h = height(sdf, bevel, dome)
        m = mask(sdf)
        lit = shade(h, base, gloss=1.0, scale=0.9 if dome else 1.0)
        col = col * (1 - m)[..., None] + lit * m[..., None]
        alpha = np.maximum(alpha, m)
    rgba = np.dstack([col, alpha])
    return Image.fromarray((rgba * 255).astype(np.uint8), "RGBA")


if __name__ == "__main__":
    name, out = sys.argv[1], sys.argv[2]
    size = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3].isdigit() else 512
    mark_only = "--mark" in sys.argv
    img = render(name, mark_only).resize((size, size), Image.LANCZOS)
    if not mark_only:
        img = img.convert("RGB")
    img.save(out)
