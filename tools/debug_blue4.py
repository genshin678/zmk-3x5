#!/usr/bin/env python3
import numpy as np
from PIL import Image

path = r"C:\Users\13983\Downloads\谱子\Angel_按键谱分页_第01页.png"
img = Image.open(path).convert('RGB')
arr = np.array(img)
h, w = arr.shape[:2]
print(f"Size: {w}x{h}")

# Page 1 layout: title at top, then 6 cols x 14 rows of mini-grids
# Header text "Angel" ends around y=180
# Footer text "音游伴侣生成" at bottom
# Each mini-grid is roughly 165x170px (2280 height / 14 rows = ~163, 1290 width / 6 cols = 215)
# So body ~2280x1290, 6x14 grid

# Try a 14x6 grid, each block ~163x215
body_top = 200
body_bot = h - 80
body_left = 40
body_right = w - 40
body_h = body_bot - body_top
body_w = body_right - body_left
rh = body_h / 14
cw = body_w / 6

print(f"block size ~{rh:.0f}x{cw:.0f}")

# For each mini-grid, look at the 3x5 sub-cells and report which are "active"
# Each mini-grid is 3 rows x 5 cols of cells
print("\n=== Mini-grid active map (B=blue cell, .=empty) ===")
for r in range(14):
    line = ""
    for c in range(6):
        y0 = int(body_top + r * rh)
        y1 = int(body_top + (r+1) * rh)
        x0 = int(body_left + c * cw)
        x1 = int(body_left + (c+1) * cw)
        sub = arr[y0:y1, x0:x1, :]
        sh, sw = sub.shape[:2]
        cell_h = sh / 3
        cell_w = sw / 5
        cells = ""
        for cr in range(3):
            for cc in range(5):
                cy0 = int(cr * cell_h)
                cy1 = int((cr+1) * cell_h)
                cx0 = int(cc * cell_w)
                cx1 = int((cc+1) * cell_w)
                block = sub[cy0:cy1, cx0:cx1, :].reshape(-1, 3)
                m = block.mean(axis=0)
                R, G, B = m
                # Saturated blue: B high, R low
                if B > 180 and B - R > 80 and R < 150:
                    cells += "B"
                else:
                    cells += "."
        line += f" [{cells}]"
    print(f"r{r:2d}: {line}")

# Now also: sample center pixel of first 3 blue cells we find
print("\n=== Sample blue cells (using new threshold) ===")
mask = (arr[:,:,2] > 180) & ((arr[:,:,2].astype(int) - arr[:,:,0].astype(int)) > 80) & (arr[:,:,0] < 150)
print(f"Active pixels: {mask.sum()}")
ys, xs = np.where(mask)
if len(ys) > 0:
    for i in [0, len(ys)//4, len(ys)//2, len(ys)-1]:
        y, x = ys[i], xs[i]
        rgb = arr[y, x, :]
        print(f"  ({y:4d},{x:4d}) = R{rgb[0]} G{rgb[1]} B{rgb[2]}")
