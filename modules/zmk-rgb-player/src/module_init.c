/*
 * module_init.c - bootstraps the zmk-rgb-player module.
 *
 * The module's init functions (rgb_control / effects / player / ble_service /
 * mode_c) are never invoked by ZMK automatically, so we chain them from a
 * single APPLICATION-level SYS_INIT. Idempotent; safe if others also init.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk_rgbeffect/rgb_control.h>
#include <zmk_rgbeffect/effects.h>
#include <zmk_rgbeffect/player.h>
#include <zmk_rgbeffect/ble_service.h>
#include <zmk_rgbeffect/mode_c.h>

LOG_MODULE_REGISTER(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

static int zmk_rgbeffect_init(void) {
    rgb_control_init();
    effects_init();
    player_init();
    ble_service_init();
    mode_c_init();
    LOG_INF("zmk-rgb-player module initialized");
    return 0;
}

SYS_INIT(zmk_rgbeffect_init, APPLICATION, 90);
