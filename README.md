# 3×5 蓝牙键盘 — ZMK 固件

基于 **nice!nano / Pro Micro** (nRF52840) + **ZMK Firmware**，支持 USB 有线 + 蓝牙双模。

---

## 目录结构（本仓库）

```
zmk-3x5-bt/
├── .github/workflows/build.yml   ← 官方 GitHub Actions 编译流程（一键触发）
├── build.yaml                    ← ZMK 构建矩阵（board + shield）
├── zephyr/
│   └── module.yml               ← 声明 boards/ 为板级根目录
├── config/
│   ├── west.yml                 ← 指向 zmk 仓库（revision: v0.3 稳定版）
│   ├── zmk_3x5_bt.conf          ← 用户级配置（蓝牙/睡眠/USB）
│   └── zmk_3x5_bt.keymap        ← 用户级键位（覆盖 shield 默认）
├── boards/shields/zmk_3x5_bt/    ← 自定义 shield 定义
│   ├── Kconfig.shield
│   ├── Kconfig.defconfig
│   ├── zmk_3x5_bt.overlay       ← GPIO 矩阵（3×5，COL2ROW）
│   ├── zmk_3x5_bt.keymap        ← shield 默认键位
│   └── zmk_3x5_bt.zmk.yml       ← shield 元数据
└── README.md
```

> **设计说明**：`zmk_3x5_bt` 是一个自定义 **shield**（键盘扩展板），运行在 `nice_nano` 板子（Pro Micro / nice!nano v2）上。这种结构符合 ZMK 官方规范，便于后续扩展（多 layout、RGB、split 等）。

---

## 硬件连接（必须严格对应）

```
nice!nano V2                    键盘矩阵
────────────────                ──────────────────────
D0  (P0.04)  ────────────────►  Row 0  → Y U I O P
D1  (P0.05)  ────────────────►  Row 1  → H J K L ;
D2  (P0.06)  ────────────────►  Row 2  → N M , . /
D3  (P0.22)  ◄───────────────  Col 0  ← Y H N
D4  (P0.23)  ◄───────────────  Col 1  ← U J M
D5  (P0.24)  ◄───────────────  Col 2  ← I K ,
D6  (P0.15)  ◄───────────────  Col 3  ← O L .
D7  (P0.16)  ◄───────────────  Col 4  ← P ; /
                                       ──────────────
                                  每键: 1N4148 阳→行 阴→列
D8  (P0.13)  ────────────────►  WS2812B Data (状态灯，可选)
D20 (P0.10)  ──[SW]── GND        复位按钮
VBUS ──────────────────────►  USB-C 供电 / 充电
3V3  ──────────────────────►  WS2812B / 上拉电阻供电
GND  ──────────────────────►  地
```

> ⚠️ **GPIO 引脚验证**：`P0.04/P0.05/P0.06` 等是 nice!nano 的 `D0/D1/D2` 物理映射，来自官方 Pro Micro 兼容定义。若你的板子引脚标号不同，请以实际丝印为准。

---

## 二极管方向（COL2ROW 模式）

```
Row 线（水平，从 nice!nano D0/D1/D2 发出）
    │
    │  ◄── 阳极（有色环，1N4148）
    ▼
  [SW] 开关
    │
    │  ──► 阴极（无色环）──► Col 线（垂直，到 D3-D7）
    ▼

扫描逻辑：列输出高 → 行内置下拉读取（diode-direction = "col2row"）
```

---

## 键位布局（单 layer）

```
Y       U       I       O       P
H       J       K       L       SCLN
N       M       COMMA   DOT     SLSH
```

共 15 键，纯单层设计（无 Fn / 组合键）。

如需扩展（字母层、数字层、符号层），编辑 `config/zmk_3x5_bt.keymap` 或 `boards/shields/zmk_3x5_bt/zmk_3x5_bt.keymap`。

---

## 编译与烧录

### 方法一：GitHub Actions 在线编译（推荐）

1. 将本仓库完整结构推送到 GitHub（`genshin678/zmk-3x5-bt`）。
2. 每次 push 到 `main`，GitHub Actions 自动触发编译（使用官方 `build-user-config.yml@v0.3`）。
3. 进入 **Actions** 页面 → 最新 run → **Artifacts** 下载 `firmware.zip`。
4. 解压得到 `zmk_3x5_bt-nice_nano-zmk.uf2`。

### 方法二：本地编译

```bash
# 依赖：West + Zephyr SDK（参考 https://zmk.dev/docs/development/local-toolchain/setup）
cd zmk-3x5-bt
west init -l config
west update
west zephyr-export
west build -s zmk/app -d build -b nice_nano//zmk -- -DSHIELD=zmk_3x5_bt -DZMK_CONFIG=$PWD/config
# 固件输出: build/zephyr/zmk.uf2
```

### 烧录固件

1. 双击 nice!nano 复位按钮，进入 DFU 模式（出现 `UF2BOOT` 磁盘）。
2. 将 `zmk.uf2` 复制到 `UF2BOOT` 磁盘，完成烧录。

---

## 蓝牙配对

nice!nano 上电后自动广播。在电脑/手机端搜索并配对即可。默认支持 5 个蓝牙 profile，可通过 ZMK Studio 或 `&bt` 行为切换（如需扩展）。

---

## 常见问题

**Q: 编译报错 `Could not find a package configuration file provided by "Zephyr"`？**
A: 缺少 `west zephyr-export` 步骤。本仓库使用官方 workflow，已包含该步骤。

**Q: 按键无反应？**
A: 检查二极管方向是否为 `col2row`，以及 Row/Col 引脚是否与 overlay 一致（D0-D2 为 row，D3-D7 为 col）。

**Q: nice!nano 怎么进 DFU 模式？**
A: 快速双击复位按钮，出现 `UF2BOOT` 磁盘。

**Q: WS2812B 灯不亮？**
A: 当前 overlay 未启用 RGB。如需启用，在 `zmk_3x5_bt.overlay` 添加 `worldsemi,ws2812` 节点，并在 `.conf` 开启 `CONFIG_ZMK_RGB_UNDERGLOW=y`。

---

*固件设计：ZMK Firmware | 板子：nice!nano (nRF52840) | Shield：zmk_3x5_bt | 2026-09-06*
