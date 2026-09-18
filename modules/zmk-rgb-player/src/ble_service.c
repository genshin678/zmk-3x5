/*
 * ble_service.c - ZMK Player custom BLE GATT service
 *
 * 5 characteristics:
 *   BE01 Score Upload    WRITE (append chunks to staging buffer)
 *   BE02 Playback Ctrl  WRITE (PLAY/PAUSE/STOP/MODE/CLEAR/LOAD_DONE + Mode C)
 *   BE03 Live Keypress  WRITE (1..15, triggers ripple at that key)
 *   BE04 Status         READ+NOTIFY (state, mode, position_ms, step)
 *   BE05 Events         NOTIFY (Mode C: HIT/MISS/TIMEOUT/DONE)
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zmk_rgbeffect/ble_service.h>
#include <zmk_rgbeffect/player.h>
#include <zmk_rgbeffect/effects.h>
#include <zmk_rgbeffect/mode_c.h>

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

#define SCORE_STAGING_BYTES  (16 * 1024)
static uint8_t   staging[SCORE_STAGING_BYTES];
static uint16_t  staging_len = 0;

static uint8_t   status_buf[9];   /* bytes 0..7 status; byte 8 = fw_flags */
static bool      status_notify_enabled;

static uint8_t   events_buf[4];
static bool      events_notify_enabled;

/* Send a notification on the characteristic identified by its 16-bit UUID.
 * Looks the value attribute up in the local GATT DB (our per-characteristic
 * UUIDs are unique), which is the correct way to drive bt_gatt_notify. */
/* Live keypress auto-release: a GATT callback must not sleep, so we
 * schedule a short work to release the key's ripple after ~80ms. */
static int8_t keypress_key = -1;
static struct k_work_delayable keypress_up_work;
static void keypress_up_work_fn(struct k_work *w) {
    if (keypress_key >= 0) {
        effects_on_key_up(keypress_key);
        keypress_key = -1;
    }
}

static void gatt_notify_u16(const struct bt_uuid *chrc_uuid, const void *data, uint16_t len) {
    const struct bt_gatt_attr *attr =
        bt_gatt_find_by_uuid(NULL, 0, chrc_uuid);
    if (attr == NULL) return;
    bt_gatt_notify(NULL, attr, data, len);
}

static void status_rebuild(void) {
    status_buf[0] = (uint8_t)player_get_state();
    if (mode_c_is_active()) {
        status_buf[1] = 3;   /* mode 3 = Mode C running */
    } else {
        status_buf[1] = (uint8_t)player_get_mode();
    }
    uint32_t pos = player_get_position_ms();
    memcpy(&status_buf[2], &pos, 4);
    uint16_t step = mode_c_is_active() ? mode_c_current_step() : 0;
    memcpy(&status_buf[6], &step, 2);
    /* Capability flags so the App can negotiate features without a version query.
     * bit0: wide delta (delta not clamped to 1s on a HIT). */
    status_buf[8] = (uint8_t)ZMK_RGB_PLAYER_FW_FLAGS;
}

static void status_notify(void) {
    if (!status_notify_enabled) return;
    status_rebuild();
    gatt_notify_u16(ZMK_PLAYER_CHRC_STATUS, status_buf, sizeof(status_buf));
}

/* --- BE01: Score chunk upload (append-only) --- */
static ssize_t on_score_write(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr,
                              const void *buf, uint16_t len,
                              uint16_t offset, uint8_t flags) {
    if (staging_len + len > SCORE_STAGING_BYTES) {
        LOG_WRN("score staging overflow");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    memcpy(staging + staging_len, buf, len);
    staging_len += len;
    LOG_INF("score +%u (total %u)", len, staging_len);
    return len;
}

/* --- BE02: Playback control (Player A/B + Mode C) --- */
static ssize_t on_control_write(struct bt_conn *conn,
                                const struct bt_gatt_attr *attr,
                                const void *buf, uint16_t len,
                                uint16_t offset, uint8_t flags) {
    if (len < 1) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    const uint8_t *p = (const uint8_t *)buf;
    uint8_t cmd = p[0];

    switch (cmd) {
        /* --- Player A/B --- */
        case PLAYER_CTRL_PLAY:      player_play();                       break;
        case PLAYER_CTRL_PAUSE:    player_pause();                      break;
        case PLAYER_CTRL_STOP:     player_stop();                       break;
        case PLAYER_CTRL_MODE_A:  player_set_mode(PLAYER_MODE_A);      break;
        case PLAYER_CTRL_MODE_B:  player_set_mode(PLAYER_MODE_B);      break;
        case PLAYER_CTRL_CLEAR:   player_clear();   staging_len = 0;   break;
        case PLAYER_CTRL_LOAD_DONE:
            player_load(staging, staging_len);
            staging_len = 0;
            break;

        /* --- Mode C (Assisted Play-Along) --- */
        case MODE_C_START:
            if (len < 3) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
            mode_c_start((uint16_t)(p[1] | (p[2] << 8)));
            break;
        case MODE_C_PUSH:
            if (len < 5) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
            mode_c_push((uint16_t)(p[1] | (p[2] << 8)), p[3], p[4]);
            break;
        case MODE_C_TICK:
            if (len < 5) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
            mode_c_tick((uint32_t)(p[1] | (p[2] << 8) |
                                   ((uint32_t)p[3] << 16) |
                                   ((uint32_t)p[4] << 24)));
            break;
        case MODE_C_STOP:
            mode_c_stop();
            break;

        default:
            LOG_WRN("unknown ctrl 0x%02x", cmd);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    status_notify();
    return len;
}

/* --- BE03: Live keypress (phone -> keyboard ripple) --- */
static ssize_t on_keypress_write(struct bt_conn *conn,
                                 const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len,
                                 uint16_t offset, uint8_t flags) {
    if (len < 1) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    uint8_t k = ((const uint8_t *)buf)[0];
    if (k < 1 || k > 15) return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    /* Sets last_key so the ripple wave starts from this key position.
     * The effects tick renders the wave on the next 10ms cycle.
     * TODO: schedule effects_on_key_up(k-1) via k_work_delayable(60ms)
     *       because k_msleep inside a GATT callback is unsafe. */
    effects_on_key_down(k - 1);
    keypress_key = (int8_t)(k - 1);
    k_work_schedule(&keypress_up_work, K_MSEC(80));
    return len;
}

/* --- BE04: Status read / notify --- */
static ssize_t on_status_read(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset) {
    status_rebuild();
    return bt_gatt_attr_read(conn, attr, buf, len, offset, status_buf, sizeof(status_buf));
}
static void on_status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    status_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

/* --- BE05: Mode C Events notify --- */
static void on_events_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    events_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

/* Implemented here (it owns the GATT attr); called by mode_c.c. */
void mode_c_notify_event(uint8_t event, uint8_t key, uint16_t step) {
    if (!events_notify_enabled) return;
    events_buf[0] = event;
    events_buf[1] = key;
    events_buf[2] = (uint8_t)(step & 0xFF);
    events_buf[3] = (uint8_t)((step >> 8) & 0xFF);
    gatt_notify_u16(ZMK_PLAYER_CHRC_EVENTS, events_buf, sizeof(events_buf));
}

/* --- GATT service registration --- */
BT_GATT_SERVICE_DEFINE(zmk_player_svc,
    BT_GATT_PRIMARY_SERVICE(ZMK_PLAYER_SERVICE_UUID),

    /* Score Upload (WRITE) */
    BT_GATT_CHARACTERISTIC(ZMK_PLAYER_CHRC_SCORE,
        BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL, on_score_write, NULL),

    /* Playback Control (WRITE) */
    BT_GATT_CHARACTERISTIC(ZMK_PLAYER_CHRC_CONTROL,
        BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL, on_control_write, NULL),

    /* Live Keypress (WRITE) */
    BT_GATT_CHARACTERISTIC(ZMK_PLAYER_CHRC_KEYPRESS,
        BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL, on_keypress_write, NULL),

    /* Status (READ + NOTIFY) */
    BT_GATT_CHARACTERISTIC(ZMK_PLAYER_CHRC_STATUS,
        BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_READ, on_status_read, NULL, NULL),
    BT_GATT_CCC(on_status_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* Mode C Events (NOTIFY) */
    BT_GATT_CHARACTERISTIC(ZMK_PLAYER_CHRC_EVENTS,
        BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL, NULL, NULL),
    BT_GATT_CCC(on_events_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

int ble_service_init(void) {
    /* GATT service is statically declared with BT_GATT_SERVICE_DEFINE and
     * auto-registered at boot (CONFIG_BT_GATT_DYNAMIC_DB=n). No manual
     * call needed — and indeed impossible: with DYNAMIC_DB=n the static
     * macro emits a `struct bt_gatt_service_static`, which doesn't match
     * the `struct bt_gatt_service *` arg of bt_gatt_service_register. */
    k_work_init_delayable(&keypress_up_work, keypress_up_work_fn);
    status_rebuild();
    return 0;
}
