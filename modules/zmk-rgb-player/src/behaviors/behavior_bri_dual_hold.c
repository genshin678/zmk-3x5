/*
 * behavior_bri_dual_hold.c - brightness loop when Y+P held together
 *
 * Bound to Y (pos 0) and P (pos 4) via held-combo in the keymap.
 * On press of either key:
 *   - schedule a 100ms delayed work
 *   - if the other key is already held -> work fires immediately
 * On release of either key: cancel the loop
 *
 * The combo suppresses the normal binding of BOTH keys while held,
 * so neither Y nor P sends a letter while adjusting brightness.
 */
#define DT_DRV_COMPAT zmk_behavior_bri_dual_hold

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <drivers/behavior.h>
#include <zmk_rgbeffect/rgb_control.h>

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_LOG_LEVEL);

static uint8_t held_mask;          /* bit0=Y, bit1=P */
static struct k_work_delayable work;

static void fire_loop(struct k_work *work) {
    if (held_mask != 0x3) return;
    /* Both Y and P are held: start the brightness loop (upwards). */
    rgb_control_bri_loop_start(BRI_UP);
}

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    held_mask |= (1 << event.position);
    k_work_init_delayable(&work, fire_loop);
    k_work_schedule(&work, K_MSEC(100));
    return 0;
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    held_mask &= ~(1 << event.position);
    rgb_control_bri_loop_stop();
    k_work_cancel_delayable(&work);
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
