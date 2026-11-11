"""Decode mip 0 of a raw BC1 texture dumped from guest memory (SHADPS4_DEBUG_DESCRIBE_DUMP), tile mode
13 (Thin1DThin: 8x8-block micro tiles in rows, Morton order x0 y0 x1 y1 x2 y2 inside a tile).
Writes <out>_rgb.png (alpha ignored) and <out>_alpha.png, and optionally a UV crop enlarged.
usage: texdecode.py <bin> <width> <height> <pitch> <out prefix> [u0 v0 u1 v1 [scale]]"""
import sys
import numpy as np
from PIL import Image

path, width, height, pitch, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
bw, bh, bpitch = width // 4, height // 4, pitch // 4
raw = np.fromfile(path, dtype=np.uint8)
tiles_per_row = bpitch // 8

# Block (bx, by) -> byte offset.
bx, by = np.meshgrid(np.arange(bw), np.arange(bh))
x, y = bx % 8, by % 8
morton = ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3))
offset = (((by // 8) * tiles_per_row + (bx // 8)) * 64 + morton) * 8
blocks = raw[offset[..., None] + np.arange(8)]          # (bh, bw, 8)
c0 = blocks[..., 0].astype(np.uint32) | (blocks[..., 1].astype(np.uint32) << 8)
c1 = blocks[..., 2].astype(np.uint32) | (blocks[..., 3].astype(np.uint32) << 8)
idx = (blocks[..., 4].astype(np.uint32) | (blocks[..., 5].astype(np.uint32) << 8)
       | (blocks[..., 6].astype(np.uint32) << 16) | (blocks[..., 7].astype(np.uint32) << 24))


def rgb565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.float32)


p0, p1 = rgb565(c0), rgb565(c1)
four = (c0 > c1)[..., None]
p2 = np.where(four, (2 * p0 + p1) / 3, (p0 + p1) / 2)
p3 = np.where(four, (p0 + 2 * p1) / 3, 0)
palette = np.stack([p0, p1, p2, p3], 2)                  # (bh, bw, 4, 3)
alpha_pal = np.stack([np.full(c0.shape, 255), np.full(c0.shape, 255), np.full(c0.shape, 255),
                      np.where(c0 > c1, 255, 0)], 2)    # (bh, bw, 4)
rgb = np.zeros((height, width, 3), np.uint8)
alpha = np.zeros((height, width), np.uint8)
for j in range(4):
    for i in range(4):
        sel = (idx >> (2 * (4 * j + i))) & 3            # (bh, bw)
        colour = np.take_along_axis(palette, sel[..., None, None].repeat(3, -1), 2)[:, :, 0]
        rgb[j::4, i::4] = np.clip(colour + 0.5, 0, 255).astype(np.uint8)
        alpha[j::4, i::4] = np.take_along_axis(alpha_pal, sel[..., None], 2)[:, :, 0]
Image.fromarray(rgb).save(out + '_rgb.png')
Image.fromarray(alpha).save(out + '_alpha.png')
print(out + '_rgb.png', 'mean rgb', rgb.reshape(-1, 3).mean(0).round(1), 'opaque share',
      round(float((alpha > 0).mean()), 3))
if len(sys.argv) > 9:
    u0, v0, u1, v1 = (float(v) for v in sys.argv[6:10])
    scale = int(sys.argv[10]) if len(sys.argv) > 10 else 4
    box = (int(u0 * width), int(v0 * height), int(np.ceil(u1 * width)), int(np.ceil(v1 * height)))
    crop = Image.fromarray(rgb).crop(box)
    crop = crop.resize((crop.width * scale, crop.height * scale), Image.NEAREST)
    crop.save(out + '_crop.png')
    region = rgb[box[1]:box[3], box[0]:box[2]]
    print(out + '_crop.png', box, 'mean rgb in crop', region.reshape(-1, 3).mean(0).round(1),
          'max', region.reshape(-1, 3).max(0))
