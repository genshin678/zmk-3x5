/*
 * behavior_bt_clear_bonds.c
 *
 * Combo behavior: O + . held together for 2 s -> clear all BLE bonds.
 *
 * ===========================================================================
 * THIS FILE USED TO BE WRONG, AND IT COST A PAIRING ROUND
 * ===========================================================================
 * The original body was, in full:
 *
 *     bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
 *
 * That is only HALF of what ZMK means by "clearing a bond". It empties Zephyr's
 * bond store, but ZMK keeps its OWN profile table (app/src/ble.c):
 *
 *     static struct zmk_ble_profile profiles[ZMK_BLE_PROFILE_COUNT];
 *
 * and THAT table is what ZMK consults when a host asks to pair:
 *
 *     bool zmk_ble_profile_is_open(uint8_t index) {
 *         return !bt_addr_le_cmp(&profiles[index].peer, BT_ADDR_LE_ANY);
 *     }
 *     static bool pairing_allowed_for_current_profile(struct bt_conn *conn) {
 *         return zmk_ble_active_profile_is_open() || ( ... );
 *     }
 *     static enum bt_security_err auth_pairing_accept(struct bt_conn *conn, ...) {
 *         if (info.role == BT_CONN_ROLE_PERIPHERAL &&
 *             !pairing_allowed_for_current_profile(conn)) {
 *             LOG_WRN("Rejecting pairing request to taken profile %d", active_profile);
 *             return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
 *         }
 *         return BT_SECURITY_ERR_SUCCESS;
 *     }
 *
 * profiles[].peer was never reset, so the active profile still looked OCCUPIED
 * and ZMK rejected the host's pairing request at the SMP layer. The host
 * reported "cannot pair with this device" - and removing / re-adding the device
 * on the host side could not possibly have helped, because the refusal was
 * coming from the keyboard, not from the PC.
 *
 * The board even LOOKED correctly cleared: the blue LED's not-connected
 * indicator only ever consulted is_connected(), never is_open(), so "done
 * advertising a bond, now sitting there" and "genuinely open, will accept a
 * pairing" were visually identical. bringup.c now distinguishes them
 * (2 blips = open, 3 blips = taken).
 *
 * ===========================================================================
 * THE FIX
 * ===========================================================================
 * Stop hand-rolling the sequence and call ZMK's own entry point, which does the
 * whole job (app/src/ble.c):
 *
 *     static void clear_profile_bond(uint8_t profile) {
 *         if (bt_addr_le_cmp(&profiles[profile].peer, BT_ADDR_LE_ANY)) {
 *             bt_unpair(BT_ID_DEFAULT, &profiles[profile].peer);
 *             set_profile_address(profile, BT_ADDR_LE_ANY);  // <- the half we missed
 *         }
 *     }
 *     void zmk_ble_clear_all_bonds(void) {
 *         for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++)
 *             clear_profile_bond(i);
 *         zmk_ble_prof_select(0);        // back to profile 0
 *         update_advertising();          // restart GENERAL (connectable) advertising
 *     }
 *
 * This is the very function ZMK's stock `&bt BT_CLR_ALL` calls, so this combo
 * now has semantics identical to the official behavior - it is simply bound to
 * a 2-key combo instead of a dedicated key.
 *
 * After this runs, zmk_ble_profile_is_open() is true, so
 * auth_pairing_accept() returns BT_SECURITY_ERR_SUCCESS and a host can pair.
 */
#define DT_DRV_COMPAT zmk_behavior_bt_clear_bonds

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <drivers/behavior.h>
#include <zmk_rgbeffect/mode_c.h>
#include <zmk/ble.h>

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

/* The combo went from four keys (U+M+O+., which never fit inside the combo
 * timeout and was therefore dead) to two. Two keys are reachable BY ACCIDENT,
 * and this operation cannot be undone - every bond is gone and every host has
 * to be paired again. So it now needs a deliberate hold, same shape as
 * behavior_dfu_combo.c: schedule on press, cancel on release. */
#define BT_CLR_HOLD_MS 2000

static bool combo_held;
static struct k_work_delayable bt_clr_hold_work;

static void fire_clear(struct k_work *w) {
    ARG_UNUSED(w);
    if (!combo_held) {
        return;
    }
    /* NOT bt_unpair() on its own. zmk_ble_clear_all_bonds() is the complete
     * operation: for every profile it calls bt_unpair() AND
     * set_profile_address(ANY) - the second call is what marks the profile
     * "open" so ZMK stops rejecting pairing requests - then it selects
     * profile 0 and restarts general connectable advertising. */
    zmk_ble_clear_all_bonds();
}

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    /* Locked out while a score is playing: the strip and the cue belong to Mode C
     * until the user stops. See behavior_effect_cycle.c for the full reasoning. */
    if (mode_c_is_active()) {
        return 0;
    }

    combo_held = true;
    k_work_init_delayable(&bt_clr_hold_work, fire_clear);
    k_work_schedule(&bt_clr_hold_work, K_MSEC(BT_CLR_HOLD_MS));
    return 0;
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    combo_held = false;
    k_work_cancel_delayable(&bt_clr_hold_work);
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
