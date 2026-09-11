/*
 * behavior_effect_cycle.c - cycle to the next RGB effect
 *
 * Each press (combo fire) advances one step.
 * Combo suppresses all 4 corner keys while held (< 60ms hold).
 */
#include <zephyr/kernel.h>
#include <zmk/behavior.h>
#include <zmk_rgbeffect/rgb_control.h>

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event *event) {
    rgb_control_next_effect();
    return 0;
}
static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event *event) { return 0; }
static const struct zmk_behavior_driver_api api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};
ZMK_BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, void, rgb_effect_cycle, &api);