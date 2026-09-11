/*
 * behavior_hue_dual_hold.c - hue loop when H+; held together
 *
 * Bound to H (pos 5) and ; (pos 9) via held-combo.
 * Same pattern as brightness dual-hold.
 * Direction: +8 deg per 100ms (clockwise).
 */
#include <zephyr/kernel.h>
#include <zmk/behavior.h>
#include <zmk_rgbeffect/rgb_control.h>

static uint8_t held_mask;          /* bit0=H, bit1=SCLN */
static struct k_work_delayable work;

static void fire_loop(struct k_work *work) {
    if (held_mask != 0b11) return;
    rgb_control_hue_loop_start(+1);
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
    rgb_control_hue_loop_stop();
    k_work_cancel_delayable(&work);
    return 0;
}
static const struct zmk_behavior_driver_api api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};
ZMK_BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, void, rgb_hue_dual, &api);