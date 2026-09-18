#!/usr/bin/env python3
import numpy as np
from PIL import Image
from scipy import ndimage

def main():
    path = r"C:\Users\13983\Downloads\谱子\Angel_按键谱分页_第01页.png"
    img = Image.open(path).convert('RGB')
    arr = np.array(img)
    h, w = arr.shape[:2]
    r = arr[:, :, 0].astype(int)
    g = arr[:, :, 1].astype(int)
    b = arr[:, :, 2].astype(int)

    # Active cell blue: saturated blue, darker than background
    # Background: R~214 G~218 B~225 (light). Active: medium blue.
    active = (b > 160) & (b - r > 70) & (b - g > 50) & (r < 150)
    print(f"Active pixels: {active.sum()}")

    # Connected components
    labeled, n = ndimage.label(active)
    print(f"Connected components: {n}")

    # Filter by size (cells are ~20-30px, so area ~400-900)
    sizes = ndimage.sum(np.ones_like(labeled), labeled, range(1, n+1))
    valid = [i+1 for i, s in enumerate(sizes) if 200 < s < 3000]
    print(f"Valid cell-sized components: {len(valid)}")

    # Report bounding boxes and mean color of a few
    for i in valid[:20]:
        ys, xs = np.where(labeled == i)
        y0, y1 = ys.min(), ys.max()
        x0, x1 = xs.min(), xs.max()
        sub = arr[y0:y1+1, x0:x1+1, :]
        m = sub.reshape(-1, 3).mean(axis=0)
        print(f"  comp {i:3d}: bbox=({y0},{x0})-({y1},{x1}) size={len(ys)} meanRGB=({m[0]:.0f},{m[1]:.0f},{m[2]:.0f})")

if __name__ == '__main__':
    main()