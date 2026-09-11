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

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zmk/behavior.h>

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event *event) {
    return bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event *event) {
    return 0;
}

static const struct zmk_behavior_driver_api api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};

ZMK_BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, void, bt_clear_bonds, &api);
