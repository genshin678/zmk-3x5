/*
 * btdiag.c - BLE link-latency probe. TEMPORARY, gated by
 * CONFIG_ZMK_RGB_PLAYER_BTDIAG.
 *
 * THE SYMPTOM THIS EXISTS TO EXPLAIN
 * "After the keyboard has been idle for a while the FIRST key is slow; once I
 * am typing continuously it is fine." The same lag appears on a phone.
 *
 * WHAT IS ALREADY RULED OUT (each for a reason, not by assumption)
 *   - The connection interval. A slow interval makes EVERY key slow, not just
 *     the first one after a pause. The symptom's shape contradicts it.
 *   - The host. It happens on a phone too, so the host is not the variable.
 *   - Our effect engine. effects.c is inert while CONFIG_ZMK_RGB_PLAYER_BRINGUP
 *     is set (effects_set_active() returns immediately), so it was not running
 *     any render tick when the user reported the lag.
 *   - The WS2812 strip thread. Disabled since the LATFIX build.
 *   - Our own activity hooks. Nothing in this module subscribes to
 *     zmk_activity_state_changed.
 *
 * WHAT IS *NOT* RULED OUT, AND IS WHAT THIS MEASURES
 * The link parameters actually in force. ZMK only ever advertises a PREFERENCE
 * (CONFIG_BT_PERIPHERAL_PREF_* appear in the advertising PDU) and never asks
 * the host to honour it at runtime. Zephyr's host does request an update once,
 * on connect (CONFIG_BT_GAP_AUTO_UPDATE_CONN_PARAMS=y is set in this build),
 * but nothing re-asserts it later. If the host relaxes the parameters while
 * the link is idle - which is a common power-saving behaviour - then the first
 * key after a pause waits for a much later connection event, and every key
 * after that is fast again because the activity pulled the parameters back.
 * That mechanism fits the symptom exactly, and it is measurable.
 *
 * HOW TO READ THE BLUE LED (P0.15)
 *
 *   boot            : THIRTEEN quick blinks. This is the build signature and
 *                     the only reliable way to tell which build is flashed -
 *                     HSVFIX THIRTEEN, COMBOFIX2 TWELVE, BRIWRAP ELEVEN,
 *                     COMBOFIX TEN, USBRGB NINE, PAIRFIX EIGHT, LATFIX SEVEN,
 *                     KPTEST SIX, LAYERS / HIDCHK FIVE.
 *
 *   each key press  : 1..5 blinks encoding the WORST-CASE link latency, i.e.
 *                     interval x (latency + 1) - the longest a keystroke can
 *                     sit waiting for its turn on the air:
 *
 *                       1 blink   <= 10 ms    excellent
 *                       2 blinks  <= 20 ms    fine
 *                       3 blinks  <= 50 ms    perceptible
 *                       4 blinks  <= 100 ms   sluggish
 *                       5 blinks  >  100 ms   this is the reported bug
 *
 *                     One blink also means "no parameters reported yet", i.e.
 *                     the host has never sent an update - measure again.
 *
 * THE EXPERIMENT (please report both numbers)
 *   1. Type continuously for a few seconds and note the blink count.
 *   2. Leave the keyboard completely alone for ~40 s. That crosses ZMK's
 *      CONFIG_ZMK_IDLE_TIMEOUT (30000 ms), so the board is in its IDLE state.
 *   3. Press ONE key and note the count.
 *
 *   Count goes UP at step 3  -> the host relaxes the link while idle. Root
 *                               cause confirmed; the fix belongs in firmware
 *                               (re-request low latency on the first key).
 *   Same count at both steps  -> link parameters are NOT the cause. This
 *                               probe is done and the search moves elsewhere.
 *
 * A note on why the reading is also refreshed when the parameters change:
 * bd_param_cb() is the only place that sees a granted parameter set, so a
 * change (including one triggered by the host while idle) blinks immediately
 * instead of waiting for the next press.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/conn.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#include <zmk_rgbeffect/btdiag.h>

#if defined(CONFIG_ZMK_RGB_PLAYER_BTDIAG)

/* Priority 11 sits BELOW every radio path: ZMK's BLE notify thread is 5,
 * Zephyr's BT host RX is 8, the bring-up LED thread was 10, and idle is 15.
 * A diagnostic indicator must never be able to preempt the protocol stack. */
#define BD_PRIORITY     11

#define BD_ON_MS        60
#define BD_GAP_MS       60
#define BD_SETTLE_MS   400
#define BD_BOOT_BLINKS  15
#define BD_POST_BOOT_MS 600

static const struct gpio_dt_spec bd_led = GPIO_DT_SPEC_GET(DT_NODELABEL(blue_led), gpios);
static bool bd_led_ok;

/* Granted link parameters, written by the BT RX thread and read by the LED
 * thread. Each field is a 16-bit aligned store, which is atomic on Cortex-M4,
 * so no lock is needed just to report them. */
static volatile uint16_t bd_interval;   /* units of 1.25 ms                        */
static volatile uint16_t bd_latency;    /* connection events the slave may skip    */
static volatile bool     bd_have_params;

static K_SEM_DEFINE(bd_show, 0, 1);

K_THREAD_STACK_DEFINE(bd_stack, 1024);
static struct k_thread bd_thread;

/*
 * Worst-case link latency, bucketed for a human to count on an LED.
 *
 * interval is in 1.25 ms units, so the effective wait in ms is
 *     interval * (latency + 1) * 1.25
 * which is exactly how long a keystroke can wait for the next connection
 * event when the peripheral is allowed to skip `latency` of them.
 */
static uint8_t bd_bucket(void) {
    if (!bd_have_params) {
        return 1;   /* nothing granted yet - "not measured", not "good" */
    }

    uint32_t eff_units = (uint32_t)bd_interval * ((uint32_t)bd_latency + 1u);
    uint32_t ms = (eff_units * 5u) / 4u;   /* 1.25 ms per unit */

    if (ms <= 10) {
        return 1;
    }
    if (ms <= 20) {
        return 2;
    }
    if (ms <= 50) {
        return 3;
    }
    if (ms <= 100) {
        return 4;
    }
    return 5;
}

/* ------------------------------------------------------------------ */
/* trigger 1: the host granted (or changed) link parameters            */
/* ------------------------------------------------------------------ */
static void bd_param_cb(struct bt_conn *conn, uint16_t interval, uint16_t latency,
                        uint16_t timeout) {
    ARG_UNUSED(conn);
    ARG_UNUSED(timeout);

    bd_interval = interval;
    bd_latency = latency;
    bd_have_params = true;

    k_sem_give(&bd_show);
}

static struct bt_conn_cb bd_conn_cb = {
    .le_param_updated = bd_param_cb,
};

/* ------------------------------------------------------------------ */
/* trigger 2: a key press - latch only, never block the kscan context   */
/* ------------------------------------------------------------------ */
static int bd_position_cb(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev != NULL && ev->state) {     /* press, not release */
        k_sem_give(&bd_show);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(bd_position, bd_position_cb);
ZMK_SUBSCRIPTION(bd_position, zmk_position_state_changed);

/* ------------------------------------------------------------------ */
/* read-out                                                            */
/* ------------------------------------------------------------------ */
static void bd_set(bool on) {
    if (bd_led_ok) {
        gpio_pin_set_dt(&bd_led, on ? 1 : 0);
    }
}

static void bd_thread_fn(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    /* Boot signature: the count is the only build fingerprint available on a
     * board with no serial console wired up. */
    for (uint8_t i = 0; i < BD_BOOT_BLINKS; i++) {
        bd_set(true);
        k_sleep(K_MSEC(BD_ON_MS));
        bd_set(false);
        k_sleep(K_MSEC(BD_GAP_MS));
    }
    bd_set(false);
    k_sleep(K_MSEC(BD_POST_BOOT_MS));

    for (;;) {
        if (k_sem_take(&bd_show, K_FOREVER) != 0) {
            continue;
        }

        /* Collapse a queue of triggers into one reading. Typing fast would
         * otherwise produce an unreadable strobe. The semaphore's limit of 1
         * already means "at least one pending". */
        k_sem_reset(&bd_show);

        uint8_t n = bd_bucket();

        for (uint8_t i = 0; i < n; i++) {
            bd_set(true);
            k_sleep(K_MSEC(BD_ON_MS));
            bd_set(false);
            if (i + 1 < n) {
                k_sleep(K_MSEC(BD_GAP_MS));
            }
        }

        k_sleep(K_MSEC(BD_SETTLE_MS));
    }
}

void btdiag_init(void) {
    if (!device_is_ready(bd_led.port)) {
        bd_led_ok = false;
        return;
    }

    bd_led_ok = (gpio_pin_configure_dt(&bd_led, GPIO_OUTPUT_INACTIVE) == 0);
    if (!bd_led_ok) {
        return;
    }

    /* Register before any connection can be established. This runs from the
     * module's APPLICATION-priority SYS_INIT, which is after bt_enable() has
     * started but long before a host can have connected. */
    bt_conn_cb_register(&bd_conn_cb);

    k_thread_create(&bd_thread, bd_stack, K_THREAD_STACK_SIZEOF(bd_stack),
                    bd_thread_fn, NULL, NULL, NULL, BD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&bd_thread, "bd_led");
}

#else   /* !CONFIG_ZMK_RGB_PLAYER_BTDIAG */

void btdiag_init(void) {}

#endif  /* CONFIG_ZMK_RGB_PLAYER_BTDIAG */
