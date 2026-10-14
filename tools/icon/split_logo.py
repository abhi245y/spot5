#!/usr/bin/env python3
# ps5-spotify - Splits the Spot5 mark into the app's logo layers.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: split_logo.py <mark.png (transparent)> <out dir>
# Writes arc-0.png (top) .. arc-2.png (bottom), each the full 512x512 frame
# holding one arc, and shine.png: the whole mark as a white silhouette (the
# light sweep). Also prints each arc's centre as a fraction of the frame.
import collections
import sys
import numpy as np
from PIL import Image, ImageFilter

src, out = sys.argv[1:]
im = Image.open(src).convert("RGBA")
a = np.asarray(im).copy()
a[..., 3] = np.where(a[..., 3] < 24, 0, a[..., 3])
im = Image.fromarray(a).crop(Image.fromarray(a).getbbox())
side = max(im.size)
square = Image.new("RGBA", (side, side))
square.paste(im, ((side - im.width) // 2, (side - im.height) // 2))
square = square.resize((512, 512), Image.LANCZOS)
rgba = np.asarray(square)
core = rgba[..., 3] > 128

labels = np.zeros(core.shape, int)
count = 0
for y0, x0 in zip(*np.nonzero(core)):
    if labels[y0, x0]:
        continue
    count += 1
    queue = collections.deque([(y0, x0)])
    labels[y0, x0] = count
    while queue:
        y, x = queue.popleft()
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            yy, xx = y + dy, x + dx
            if 0 <= yy < 512 and 0 <= xx < 512 and core[yy, xx] and not labels[yy, xx]:
                labels[yy, xx] = count
                queue.append((yy, xx))
assert count == 3, f"expected three arcs, found {count}"
# Top arc first: order by each component's top edge.
order = sorted(range(1, 4), key=lambda n: np.nonzero(labels == n)[0].min())
for index, n in enumerate(order):
    grown = Image.fromarray(((labels == n) * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(9))
    keep = np.asarray(grown) > 0
    layer = rgba.copy()
    layer[..., 3] = np.where(keep, layer[..., 3], 0)
    layer[layer[..., 3] == 0] = 0  # clear hidden colour: smaller files
    Image.fromarray(layer).save(f"{out}/arc-{index}.png", optimize=True)
    ys, xs = np.nonzero(labels == n)
    print(f"arc-{index} centre {(xs.min() + xs.max()) / 1024:.3f} {(ys.min() + ys.max()) / 1024:.3f}")
shine = np.zeros_like(rgba)
shine[..., :3] = 255
shine[..., 3] = rgba[..., 3]
Image.fromarray(shine).save(f"{out}/shine.png", optimize=True)
