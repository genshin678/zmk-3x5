#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
解析"光遇 - 按键谱"分页 PNG 图片。

每张图：
  - 6 列 x 14 行 = 84 个 mini-grid
  - 每个 mini-grid = 3 行 x 5 列 = 15 个格子，对应物理 3x5 键盘
  - 蓝色格子 = 当前步要按的键

输出 JSON 格式：
  {
    "pages": [
      {
        "page": 1,
        "steps": [
          {"step": 0, "keys": [1, 5, 10]},   # keyIndex 1-15, 行优先
          ...
        ]
      },
      ...
    ],
    "total_steps": N
  }

keyIndex 映射（行优先 1-15）：
  Row 0 (Y U I O P) = 1  2  3  4  5
  Row 1 (H J K L ;) = 6  7  8  9  10
  Row 2 (N M , . /) = 11 12 13 14 15
"""

import sys
import json
import glob
import os
import numpy as np
from PIL import Image


def parse_page(path):
    """解析一张图片，返回 steps 列表。每个 step 是 keyIndex 列表。"""
    img = Image.open(path).convert('RGB')
    arr = np.array(img)
    h, w = arr.shape[:2]

    # 布局常量（已校准）
    body_top = 200
    body_bot = h - 80
    body_left = 40
    body_right = w - 40
    body_h = body_bot - body_top
    body_w = body_right - body_left
    rh = body_h / 14   # 每行 mini-grid 高度
    cw = body_w / 6    # 每列 mini-grid 宽度

    steps = []
    step_idx = 0

    # 扫描顺序：逐行扫描（从左到右，从上到下）
    for r in range(14):
        for c in range(6):
            y0 = int(body_top + r * rh)
            y1 = int(body_top + (r + 1) * rh)
            x0 = int(body_left + c * cw)
            x1 = int(body_left + (c + 1) * cw)
            sub = arr[y0:y1, x0:x1, :]
            sh, sw = sub.shape[:2]
            cell_h = sh / 3
            cell_w = sw / 5

            keys = []
            for cr in range(3):
                for cc in range(5):
                    cy0 = int(cr * cell_h)
                    cy1 = int((cr + 1) * cell_h)
                    cx0 = int(cc * cell_w)
                    cx1 = int((cc + 1) * cell_w)
                    block = sub[cy0:cy1, cx0:cx1, :].reshape(-1, 3)
                    m = block.mean(axis=0)
                    R, G, B = float(m[0]), float(m[1]), float(m[2])
                    # 蓝色阈值（已校准：纯蓝 R42 G111 B244）
                    if B > 180 and (B - R) > 80 and R < 150:
                        key_idx = cr * 5 + cc + 1   # 1-15 行优先
                        keys.append(key_idx)

            # 即使本 mini-grid 没有蓝格，也算一个空 step（保持节拍连续）
            # 但如果整页都是空的（封面/尾页），就跳过
            steps.append({"step": step_idx, "keys": keys})
            step_idx += 1

    return steps


def main():
    if len(sys.argv) < 2:
        # 默认目录
        src_dir = r"C:\Users\13983\Downloads\谱子"
    else:
        src_dir = sys.argv[1]

    files = sorted(glob.glob(os.path.join(src_dir, "Angel_按键谱分页_第*.png")))
    if not files:
        print(f"No PNG files found in {src_dir}", file=sys.stderr)
        sys.exit(1)

    print(f"Found {len(files)} pages", file=sys.stderr)

    pages = []
    total_steps = 0
    for i, f in enumerate(files, 1):
        steps = parse_page(f)
        # 过滤：如果整页蓝格都为 0，可能是空白页
        non_empty = sum(1 for s in steps if s["keys"])
        if non_empty == 0:
            print(f"  Page {i}: empty (skipped)", file=sys.stderr)
            continue
        # 重编号 step
        for s in steps:
            s["step"] = total_steps
            total_steps += 1
        pages.append({"page": i, "file": os.path.basename(f), "steps": steps})
        print(f"  Page {i}: {non_empty}/{len(steps)} non-empty steps", file=sys.stderr)

    out = {
        "format": "image-score-v1",
        "source": "Angel_按键谱分页",
        "pages": pages,
        "total_steps": total_steps,
        "key_map": {
            "1": "Y", "2": "U", "3": "I", "4": "O", "5": "P",
            "6": "H", "7": "J", "8": "K", "9": "L", "10": ";",
            "11": "N", "12": "M", "13": ",", "14": ".", "15": "/"
        }
    }

    out_path = os.path.join(os.path.dirname(__file__), "image-score.json")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=2)
    print(f"Wrote {out_path} ({total_steps} steps)", file=sys.stderr)


if __name__ == "__main__":
    main()
