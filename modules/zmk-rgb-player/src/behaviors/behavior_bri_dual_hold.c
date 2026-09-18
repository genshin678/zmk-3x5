/*
 * behavior_bri_dual_hold.c - brightness loop when Y+P held together
 *
 * Bound to the Y+P combo (key-positions <0 4>) in the keymap.
 *
 * IMPORTANT — how ZMK invokes a combo behavior:
 *   A combo fires ONCE for the whole combo and ZMK passes
 *   event.position = ZMK_VIRTUAL_KEY_POSITION_COMBO(idx), i.e. a *virtual*
 *   position (keymap_len + combo index), NOT the physical key position.
 *   The individual member keys' press events are consumed by the combo.
 *   Therefore a per-physical-position held_mask (as an earlier revision used:
 *   `held_mask |= (1 << event.position)` + `if (held_mask != 0x3)`) can never
 *   work — the shift overflows a uint8_t and truncates to 0.
 *   We simply track "the combo is currently held" with a bool.
 *
 * On combo press : schedule a 100 ms delay; if still held -> start the loop.
 * On combo release: cancel the loop.
 *
 * The combo suppresses the normal binding of BOTH keys while held, so neither
 * Y nor P sends a letter while adjusting brightness.
 */
#define DT_DRV_COMPAT zmk_behavior_bri_dual_hold

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <zmk_rgbeffect/mode_c.h>
#include <zmk_rgbeffect/rgb_control.h>

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

static bool combo_held;
static struct k_work_delayable bri_hold_work;

static void fire_loop(struct k_work *w) {
    ARG_UNUSED(w);
    if (!combo_held) {
        return;
    }
    /* Combo confirmed held: start the brightness loop (upwards). */
    rgb_control_bri_loop_start(BRI_UP);
}

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    /* Locked out while a score is playing: the strip and the cue belong to Mode C
     * until the user stops. See behavior_effect_cycle.c for the full reasoning. */
    if (mode_c_is_active()) {
        return 0;
    }
    combo_held = true;
    k_work_init_delayable(&bri_hold_work, fire_loop);
    k_work_schedule(&bri_hold_work, K_MSEC(100));
    return 0;
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    combo_held = false;
    rgb_control_bri_loop_stop();
    k_work_cancel_delayable(&bri_hold_work);
    return 0;
}

static const struct behavior_driver_api api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};

#define BRI_DUAL_INST(n)                                                                           \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &api);

DT_INST_FOREACH_STATUS_OKAY(BRI_DUAL_INST)
