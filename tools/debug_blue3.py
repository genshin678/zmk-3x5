#!/usr/bin/env python3
"""
debug_blue3.py - Pure PIL/numpy, no scipy.
Sample the image, report where "active blue" cells are by scanning
a coarse grid and printing mean RGB of each block.
"""
import numpy as np
from PIL import Image

path = r"C:\Users\13983\Downloads\谱子\Angel_按键谱分页_第01页.png"
img = Image.open(path).convert('RGB')
arr = np.array(img)
h, w = arr.shape[:2]
print(f"Size: {w}x{h}")

# Scan a 12x6 grid (rows x cols) over the body, report mean color of each block
# and whether it looks "active blue".
# Body: top=180, bottom=h-80, left=40, right=w-40
body_top, body_bot = 180, h - 80
body_left, body_right = 40, w - 40
body_h = body_bot - body_top
body_w = body_right - body_left

# Try a finer grid: 24 rows x 6 cols (each mini-grid is ~2 blocks tall)
ROWS, COLS = 24, 6
rh = body_h / ROWS
cw = body_w / COLS

print(f"\nGrid {ROWS}x{COLS}, block size ~{rh:.0f}x{cw:.0f}")
print("\nMean RGB per block (R G B)  ->  active if B-R>50 & B-G>30 & R<160:")
for r in range(ROWS):
    row_str = ""
    for c in range(COLS):
        y0 = int(body_top + r * rh)
        y1 = int(body_top + (r+1) * rh)
        x0 = int(body_left + c * cw)
        x1 = int(body_left + (c+1) * cw)
        block = arr[y0:y1, x0:x1, :].reshape(-1, 3)
        m = block.mean(axis=0)
        R, G, B = int(m[0]), int(m[1]), int(m[2])
        active = (B - R > 50) and (B - G > 30) and (R < 160)
        if active:
            row_str += f"  [{R:3d},{G:3d},{B:3d}]"
        else:
            row_str += f"  .        "
    print(f"  r{r:2d}: {row_str}")
