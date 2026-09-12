/*
 * behavior_bt_clear_bonds.c
 *
 * Combo behavior: U + M + O + . pressed together -> clear all BLE bonds.
 *
 * Calls bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY) to wipe every stored
 * pairing on the keyboard. After this, ZMK returns to general (discoverable)
 * advertising and any phone can find and pair with the keyboard again.
 *
 * This recovers from the state where the keyboard only reconnects to
 * already-paired hosts and is invisible to new Bluetooth scans.
 */
#define DT_DRV_COMPAT zmk_behavior_bt_clear_bonds

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <drivers/behavior.h>

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    return bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    return 0;
}

static const struct behavior_driver_api api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};

#define BT_CLEAR_INST(n)                                                                           \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &api);

DT_INST_FOREACH_STATUS_OKAY(BT_CLEAR_INST)
