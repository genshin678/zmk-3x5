cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS {ZEPHYR_BASE})
target_sources(app PRIVATE
    src/led_pixel.c
    src/effects.c
    src/player.c
    src/ble_service.c
    src/rgb_control.c
    src/dfu.c
    src/cdc.c
    src/mode_c.c
    src/bringup.c
    src/module_init.c
    src/behaviors/behavior_bri_dual_hold.c
    src/behaviors/behavior_hue_dual_hold.c
    src/behaviors/behavior_effect_cycle.c
    src/behaviors/behavior_dfu_combo.c
    src/behaviors/behavior_kp_with_effect.c
    src/behaviors/behavior_bt_clear_bonds.c
)

target_include_directories(app PRIVATE include)
