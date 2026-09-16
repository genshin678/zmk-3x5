/*
 * behavior_hue_dual_hold.c - hue loop when H+; held together
 *
 * Bound to the H+; combo (key-positions <5 9>) in the keymap.
 * Same pattern as the brightness dual-hold: ZMK fires a combo behavior once
 * with a *virtual* event.position, so we track "combo held" with a bool
 * instead of a per-physical-position bitmask (see behavior_bri_dual_hold.c
 * for the full explanation).
 *
 * Direction: +8 deg per 100ms (clockwise).
 */
#define DT_DRV_COMPAT zmk_behavior_hue_dual_hold

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <zmk_rgbeffect/rgb_control.h>

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

static bool combo_held;
static struct k_work_delayable hue_hold_work;

static void fire_loop(struct k_work *w) {
    ARG_UNUSED(w);
    if (!combo_held) {
        return;
    }
    rgb_control_hue_loop_start(+1);
}

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    combo_held = true;
    k_work_init_delayable(&hue_hold_work, fire_loop);
    k_work_schedule(&hue_hold_work, K_MSEC(100));
    return 0;
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    combo_held = false;
    rgb_control_hue_loop_stop();
    k_work_cancel_delayable(&hue_hold_work);
    return 0;
}

static const struct behavior_driver_api api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};

#define HUE_DUAL_INST(n)                                                                           \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &api);

DT_INST_FOREACH_STATUS_OKAY(HUE_DUAL_INST)
