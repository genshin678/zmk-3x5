/*
 * behavior_out_guard.c - &out OUT_TOG, but inert while a score is playing
 *
 * The out_combo (I+K) flips the active endpoint between USB and Bluetooth, which is
 * genuinely needed on this board (plugged in for power, ZMK prefers USB, so keystrokes
 * stop reaching the Bluetooth host until the user flips it back).
 *
 * The problem is that I and K are also two of the fifteen keys a score can ask for. A
 * stock &out cannot be made conditional, so a slow passage - where the user rests long
 * enough for the combo's require-prior-idle-ms window to open, then presses I+K together
 * because the sheet says so - would switch the transport and silently drop the link the
 * notes are travelling over. Mid-song that is unrecoverable: the board keeps playing to
 * nowhere.
 *
 * This behaviour does exactly what the stock one does, except that it refuses while
 * Mode C is running. Playback ends (or the user stops), and the combo works again.
 */
#define DT_DRV_COMPAT zmk_behavior_out_guard

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <zmk/endpoints.h>
#include <zmk_rgbeffect/mode_c.h>

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    if (mode_c_is_active()) {
        LOG_INF("out_guard: ignored, a score is playing");
        return 0;
    }
    return zmk_endpoints_toggle_transport();
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return 0;
}

static const struct behavior_driver_api api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};

#define OUT_GUARD_INST(n)                                                                          \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &api);

DT_INST_FOREACH_STATUS_OKAY(OUT_GUARD_INST)
