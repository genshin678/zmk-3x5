/*
 * behavior_user_kp.c - runtime-remappable manual key behavior
 *
 * Drop-in replacement for ZMK's stock &kp on the 15 main keys. On press/release
 * it emits the HID usage stored in user_keymap_table[event.position] instead of
 * a compile-time binding, so the mapping is whatever the user configured (over
 * BLE, default = 光遇 layout).
 *
 * Combos still work: when a combo captures a key's position events the behavior
 * is never invoked, exactly as with stock &kp, so corner/side keys that double
 * as combo triggers keep their letter suppressed during a chord.
 *
 * Per-key LED feedback is driven independently off the kscan position event
 * (see effects.c), so this behavior only deals with HID output.
 */
#define DT_DRV_COMPAT zmk_behavior_user_kp

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <zmk/hid.h>
#include <zmk/endpoints.h>
#include <zmk_rgbeffect/user_keymap.h>

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    uint8_t code = user_keymap_get_slot((uint8_t)event.position);
    if (code == 0) return 0; /* slot disabled: emit nothing */
    zmk_hid_keyboard_press(code);
    zmk_endpoints_send_report(0x07); /* HID keyboard usage page */
    return 0;
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    uint8_t code = user_keymap_get_slot((uint8_t)event.position);
    if (code == 0) return 0;
    zmk_hid_keyboard_release(code);
    zmk_endpoints_send_report(0x07);
    return 0;
}

static const struct behavior_driver_api api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};

#define USER_KP_INST(n)                                                                        \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                            \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &api);

DT_INST_FOREACH_STATUS_OKAY(USER_KP_INST)
