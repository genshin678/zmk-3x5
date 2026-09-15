/*
 * behavior_kp_with_effect.c - type the key AND trigger ripple effect
 *
 * RETIRED FROM THE TYPING PATH (KPTEST round): no binding references this
 * behaviour any more - the keymap uses ZMK's stock &kp. It is kept compiled so
 * the ripple-capable variant stays available to re-attach later.
 *
 * For each of the 15 physical keys, this behavior:
 *   1. Sends the corresponding HID *usage ID* to the host.
 *      NOT a "scancode" and NOT (usage - 4): zmk_hid_keyboard_press() takes the
 *      raw usage ID from the Keyboard/Keypad page, e.g. I = 0x0C, N = 0x11,
 *      M = 0x10. Feeding it (usage - 4) is the bug that once made N type J and
 *      M type I. Prefer dt-bindings/zmk/keys.h symbols when this is put back
 *      into service.
 *   2. Calls effects_on_key_down(idx) so the ripple starts from that key
 *
 * #binding-cells = <2>:  arg0 = HID usage ID, arg1 = key index (0-14)
 *
 * Usage example for Y (pos 0, usage ID 0x1C, keyIndex 0):
 *   &kp_we 0x1C 0
 */
#define DT_DRV_COMPAT zmk_behavior_kp_with_effect

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <zmk/hid.h>
#include <zmk/endpoints.h>
#include <zmk_rgbeffect/effects.h>
#include <zmk_rgbeffect/bringup.h>

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    uint8_t scancode = (uint8_t)(uint32_t)binding->param1;
    int8_t  key_idx  = (int8_t)(int32_t)binding->param2;
    zmk_hid_keyboard_press(scancode);
    zmk_endpoints_send_report(0x07); /* HID keyboard usage page */
    /* KPTEST: this behaviour is no longer referenced by the keymap - the
     * typing path is stock &kp now - but the file is kept compiled so the
     * behaviour stays available. bringup_l3_signal() is a no-op: the LED
     * thread polls the HID report itself, so there is no +50 ms sample left to
     * schedule. */
    bringup_l3_signal();
    if (key_idx >= 0) {
        effects_on_key_down(key_idx);
    }
    return 0;
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    uint8_t scancode = (uint8_t)(uint32_t)binding->param1;
    int8_t  key_idx  = (int8_t)(int32_t)binding->param2;
    zmk_hid_keyboard_release(scancode);
    zmk_endpoints_send_report(0x07); /* HID keyboard usage page */
    if (key_idx >= 0) {
        effects_on_key_up(key_idx);
    }
    return 0;
}

static const struct behavior_driver_api api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};

#define KP_WE_INST(n)                                                                              \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &api);

DT_INST_FOREACH_STATUS_OKAY(KP_WE_INST)
