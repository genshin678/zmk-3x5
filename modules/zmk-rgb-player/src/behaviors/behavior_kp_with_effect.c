/*
 * behavior_kp_with_effect.c - type the key AND trigger ripple effect
 *
 * For each of the 15 physical keys, this behavior:
 *   1. Sends the corresponding HID scancode to the host
 *   2. Calls effects_on_key_down(idx) so the ripple starts from that key
 *
 * #binding-cells = <2>:  arg0 = HID scancode, arg1 = key index (0-14)
 *
 * Usage example for Y (pos 0, scancode 0x1C, keyIndex 0):
 *   &kp_we 0x1C 0
 */
#include <zephyr/kernel.h>
#include <zmk/behavior.h>
#include <zmk/hid.h>
#include <zmk_rgbeffect/effects.h>

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event *event) {
    uint8_t scancode = (uint8_t)(uint32_t)binding->param1;
    int8_t  key_idx  = (int8_t)(int32_t)binding->param2;
    zmk_hid_keyboard_press(scancode);
    if (key_idx >= 0) {
        effects_on_key_down(key_idx);
    }
    return 0;
}
static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event *event) {
    uint8_t scancode = (uint8_t)(uint32_t)binding->param1;
    int8_t  key_idx  = (int8_t)(int32_t)binding->param2;
    zmk_hid_keyboard_release(scancode);
    if (key_idx >= 0) {
        effects_on_key_up(key_idx);
    }
    return 0;
}
static const struct zmk_behavior_driver_api api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};
ZMK_BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, int32_t, kp_with_effect, &api);