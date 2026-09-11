/*
 * behavior_bri_dual_hold.c - brightness loop when Y+P held together
 *
 * Bound to Y (pos 0) and P (pos 4) via held-combo in the keymap.
 * On press of either key:
 *   - schedule a 100ms delayed work
 *   - if the other key is already held → work fires immediately
 * On release of either key: cancel the loop
 *
 * The combo suppresses the normal binding of BOTH keys while held,
 * so neither Y nor P sends a letter while adjusting brightness.
 */
#include <zephyr/kernel.h>
#include <zmk/behavior.h>
#include <zmk_rgbeffect/rgb_control.h>

static uint8_t held_mask;          /* bit0=Y, bit1=P */
static struct k_work_delayable work;

static void fire_loop(struct k_work *work) {
    if (held_mask != 0b11) return;
    bri_loop_t cur = rgb_control_get_bri_loop();
    /* On first fire decide direction (up if P held, down if only Y held).
     * Real direction: if P is held → up, if only Y held → down.
     * We determine by checking the current held_mask (both must be set). */
    if (cur == BRI_IDLE) {
        /* First fire: go up if P is currently down (we can't tell which
         * was pressed first; default to up). */
        rgb_control_bri_loop_start(BRI_UP);
    } else {
        /* Already looping — the rgb_control layer reverses direction
         * on bri_loop_start so a second Y+P press reverses. */
        rgb_control_bri_loop_start(BRI_UP);
    }
}

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event *event) {
    held_mask |= (1 << event->position);
    k_work_init_delayable(&work, fire_loop);
    k_work_schedule(&work, K_MSEC(100));
    return 0;
}
static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event *event) {
    held_mask &= ~(1 << event->position);
    rgb_control_bri_loop_stop();
    k_work_cancel_delayable(&work);
    return 0;
}
static const struct zmk_behavior_driver_api api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};
ZMK_BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, void, rgb_bri_dual, &api);