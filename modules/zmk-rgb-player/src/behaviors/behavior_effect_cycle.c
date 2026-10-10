/*
 * behavior_effect_cycle.c - cycle to the next RGB effect
 *
 * Each press (combo fire) advances one step.
 * Combo suppresses all 4 corner keys while held (< 60ms hold).
 */
#define DT_DRV_COMPAT zmk_behavior_effect_cycle

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <zmk_rgbeffect/mode_c.h>
#include <zmk_rgbeffect/rgb_control.h>

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    /* A running score owns the strip: advancing the effect here would take the LED
     * back and the red/blue cue would vanish mid-song - exactly the bug the user
     * reported. The combo stays inert until playback stops, like a hardware
     * lock-out. Nothing is lost: the effect list is unchanged, it just cannot be
     * cycled while the user is playing. */
    if (mode_c_is_active()) {
        return 0;
    }
    rgb_control_next_effect();
    return 0;
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    return 0;
}

static const struct behavior_driver_api api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};

#define EFFECT_CYCLE_INST(n)                                                                       \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &api);

DT_INST_FOREACH_STATUS_OKAY(EFFECT_CYCLE_INST)
