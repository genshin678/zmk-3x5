#!/usr/bin/env python3
import sys
import numpy as np
from PIL import Image

def main():
    path = r"C:\Users\13983\Downloads\谱子\Angel_按键谱分页_第01页.png"
    img = Image.open(path).convert('RGB')
    arr = np.array(img)
    print(f"Image size: {img.size}")  # (W, H)
    h, w = arr.shape[:2]

    # Look at overall color distribution in the body (skip header)
    body = arr[200:h-100, 50:w-50, :]
    r = body[:, :, 0].astype(int)
    g = body[:, :, 1].astype(int)
    b = body[:, :, 2].astype(int)

    print(f"Body shape: {body.shape}")
    print(f"R mean/min/max: {r.mean():.0f}/{r.min()}/{r.max()}")
    print(f"G mean/min/max: {g.mean():.0f}/{g.min()}/{g.max()}")
    print(f"B mean/min/max: {b.mean():.0f}/{b.min()}/{b.max()}")

    # Count pixels that look "blue" (b high, r/g low-ish)
    blue_mask = (b > 120) & (b - r > 40) & (b - g > 40)
    print(f"Blue-ish pixels: {blue_mask.sum()} ({100*blue_mask.sum()/blue_mask.size:.2f}%)")

    # Also check what colors appear: sample distinct cells
    # Find connected blue regions and report their RGB
    # Simpler: report the range of b values
    # Show histogram of b channel in body
    # Use flattened arrays
    bf = b.flatten(); rf = r.flatten(); gf = g.flatten()
    hist_b = np.bincount(np.clip(bf, 0, 255), minlength=256)
    top = np.argsort(hist_b)[-10:]
    print("Top blue-channel values (value: count):", {int(v): int(hist_b[v]) for v in top})

    # Find blue-ish pixels by b > r+40 AND b > g+40 (light blue tolerant)
    bluemask2 = (b > r + 40) & (b > g + 40)
    print(f"Blue2 (b>r+40 & b>g+40) pixels: {bluemask2.sum()}")
    if bluemask2.sum() > 0:
        print(f"  Blue2 mean RGB: R={rf[bluemask2].mean():.0f} G={gf[bluemask2].mean():.0f} B={bf[bluemask2].mean():.0f}")
        print(f"  Blue2 min/max B: {bf[bluemask2].min()}/{bf[bluemask2].max()}")

    # Print a small patch of pixels to understand the layout
    # Sample every 200px vertically to see where blue appears
    print("\nScanning rows for blue presence (every 100px):")
    for y in range(0, h, 100):
        row_b = arr[y, 50:w-50, 2]
        row_r = arr[y, 50:w-50, 0]
        blue_in_row = np.sum((row_b > 120) & (row_b - row_r > 40))
        if blue_in_row > 0:
            print(f"  y={y:4d}: {blue_in_row} blue pixels in row")

if __name__ == '__main__':
    main()