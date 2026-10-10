/*
 * bringup.c - LATFIX build: the typing path works; now remove the latency.
 *
 * =========================== LATFIX (current) ============================
 * STATUS: SOLVED. Bluetooth output works and every key is correct. The old
 * "no key output" fault was a stale host-side bond - GATT connected, but the
 * host was not subscribed to HID input reports, so
 * zmk_hog_send_keyboard_report() silently went nowhere. Clearing the bonds and
 * re-pairing fixed it. The keymap is ZMK's stock &kp using keys.h symbols.
 *
 * THE OPEN PROBLEM IS NOW INPUT LATENCY. This build attacks the firmware-side
 * share of it, and nothing else:
 *
 *   1. BLE connection parameters are pinned in config/zmk_3x5_bt.conf. ZMK
 *      sets none of them, so we silently inherited its battery-first defaults
 *      from app/Kconfig: interval 7.5-15 ms and, critically,
 *      BT_PERIPHERAL_PREF_LATENCY = 30 - permission for the peripheral to skip
 *      up to 30 connection events, which at the 15 ms ceiling is a 465 ms
 *      window of not listening. MAX_INT is now 6 (7.5 ms) and LATENCY is 0.
 *   2. Logging is OFF (CONFIG_LOG=n). One ~250 character LOG_INF per keystroke
 *      was draining to an unwired UART through a polled writer: ~22 ms of CPU
 *      per key, spent on text nobody can read.
 *   3. The WS2812 strip thread is OFF (BU_ENABLE_STRIP 0). The pixels cannot
 *      light on this board anyway (rail is 3.3 V, the chip needs 3.5 V), yet
 *      the thread pushed a 7.2 ms SPI burst every 400 ms from a priority that
 *      outranked Zephyr's Bluetooth host RX thread. The blue-LED probe thread
 *      also dropped from priority 6 to 10, below the whole BLE stack.
 *
 * BOOT SIGNATURE IS SEVEN BLINKS. The count is the only reliable way to tell
 * which build is on the board: LATFIX 7, KPTEST 6, LAYERS / HIDCHK 5.
 *
 * Only the firmware half of the latency story is here. Connection parameters
 * are a PREFERENCE and hosts frequently impose their own, so LATFIX-verify.md
 * carries the equally important host-side checklist (adapter power saving,
 * 2.4 GHz contention, stale driver) plus a self-test that needs no reflash.
 *
 * =================== HISTORICAL: the KPTEST rationale ====================
 * Board state: the gated 3.3V/VCC rail is ON (ext-power polarity fixed) and
 * Bluetooth is visible to the host, but NO key produces output. The strip
 * lights are a separate matter and are now understood: the schematic legend
 * aliases 3V3 <-> VDD, so the 15 WS2812B get the gated 3.3 V rail - below the
 * 3.5 V spec floor - which is a hardware rework item, not a firmware bug.
 *
 * What the bench says so far, with the transport variable already removed
 * (CONFIG_ZMK_USB=n since BLEFIX, so BLE is always the selected endpoint):
 * a key press lights the blue LED TWICE on the LAYERS build. That proves kscan
 * fired and the hand-rolled &kp_we ran - but it does NOT prove a keycode ever
 * reached the host, and the two builds that could report on that were
 * indistinguishable on the bench (both boot-blinked FIVE times).
 *
 * THIS BUILD CHANGES TWO THINGS AND NOTHING ELSE:
 *
 *  1. The typing path is now ZMK's STOCK &kp. &kp_we is out of the keymap.
 *     Whatever the hand-rolled behaviour was or was not doing, it is no longer
 *     in the circuit, and the bindings use keys.h macros, so the
 *     boot-protocol-vs-usage-ID mistake cannot recur.
 *  2. The HID report probe is now STICKY and POLLED rather than a single
 *     sample taken 50 ms after the press. The old probe could only ever
 *     produce a false "empty" (see LAYER 3 below).
 *
 * TWO LAYERS, ONE READ-OUT
 * ------------------------
 *  L1  zmk_position_state_changed  <- raised by the kscan subsystem after the
 *      row/column scan produced a contact. This is THE PHYSICAL LAYER: it
 *      proves switch, diode, PCB trace, kscan pins, scan timing and the
 *      matrix transform all worked. It does NOT involve the keymap.
 *  L2  bringup_key_event()         <- still called by effects.c, but now
 *      LOGGED ONLY and deliberately NOT counted in the read-out. On the HIDCHK
 *      build it was the proof that the hand-rolled &kp_we ran; the keymap now
 *      uses stock &kp, so there is nothing hand-rolled left to prove and
 *      counting it would only add a second way to read the same press.
 *  L3  l3_sample()                 <- the HID keyboard report itself, polled
 *      from the LED thread every 20 ms. This is the fact that matters: it
 *      separates "the firmware built a real keycode and handed it to the BLE
 *      HOG" from "the report never filled at all".
 *        report non-empty    -> firmware is complete end-to-end. A host that
 *                               still shows nothing is a HOST / BLE-LINK
 *                               fault, not a firmware fault.
 *        report always empty -> the fault is at or below the HID report.
 *
 * ON-BOARD BLUE LED (P0.15) - its OWN thread, so a blocked SPI write can
 * neither starve it nor fake its timing. On each key press it blinks ONCE PER
 * LAYER THAT REPORTED:
 *
 *   boot              : EIGHT quick blinks    <- PAIRFIX signature. LATFIX used
 *                                              SEVEN, KPTEST SIX and LAYERS /
 *                                              HIDCHK FIVE, so the count is the
 *                                              only reliable build fingerprint
 *                                              available on the bench.
 *   idle, connected   : clean 1 Hz heartbeat  <- BLE link is up
 *   idle, NO bond, not connected : double-blip every 2 s  <- the active profile
 *                                              is OPEN, so ZMK is advertising
 *                                              generically and WILL accept a
 *                                              pairing request. This is the state
 *                                              you want while pairing. (Before
 *                                              PAIRFIX this indicator only ever
 *                                              read is_connected(), so it also
 *                                              showed a double-blip on a board
 *                                              that would REFUSE pairing.)
 *   idle, bond held, not connected : TRIPLE-blip every 2 s <- the profile still
 *                                              holds an address, so
 *                                              zmk_ble_profile_is_open() is false
 *                                              and auth_pairing_accept() returns
 *                                              BT_SECURITY_ERR_PAIR_NOT_ALLOWED.
 *                                              The host reports "cannot pair with
 *                                              this device", and NOTHING on the
 *                                              host side can fix it: the refusal
 *                                              is issued by the keyboard. Press
 *                                              U+M+O+. to clear the bonds.
 *   key press, 2 blinks : kscan AND a non-empty HID report -> the firmware is
 *                         complete end-to-end and a real keycode was handed to
 *                         the Bluetooth HOG. If the host still shows nothing,
 *                         the fault is on the HOST / BLE side. Clear the bonds
 *                         and re-pair before touching the firmware again.
 *   key press, 1 blink  : kscan ran but the report NEVER filled. With stock
 *                         &kp in the keymap, the keymap / binding / behaviour
 *                         layers are out of frame, so this points at the HID
 *                         report itself (report size / report type / the
 *                         endpoint), not at a mapping mistake.
 *   key press, 0 blinks : kscan never fired -> PHYSICAL layer: switch, diode,
 *                         PCB trace, or a kscan pin
 *
 * HOLD THE KEY DOWN FOR A FULL TWO SECONDS when you count. The report is now
 * polled across the whole window rather than sampled once, so a hold is not
 * strictly required any more - but a deliberate 2 s hold removes any remaining
 * doubt about press/release merging.
 *
 * WS2812 STRIP - own thread. Same self-describing static pattern as the
 * PATTERN build, PLUS a marker driven straight off the kscan event (so it
 * works even when the keymap is broken):
 *   dark 3.2 s -> red 6 s -> green 6 s -> blue 6 s -> checker 6 s -> loop
 *   any kscan position event -> that pixel goes WHITE for 1.2 s
 *
 * ======================= BEFORE SHIPPING CHECKLIST =======================
 *  [ ] Remove CONFIG_ZMK_RGB_PLAYER_BRINGUP (drops both diagnostic threads).
 *  [ ] Set CONFIG_LOG=y only if UART logging is genuinely wanted.
 *  [ ] Decide CONFIG_ZMK_USB. Leaving it n makes this a pure-Bluetooth board
 *      and removes ZMK's endpoint-selection trap for good; setting it back to
 *      y re-adds USB HID, but remember ZMK hands each report to exactly ONE
 *      transport and the default preference when both are live is USB.
 *  [ ] Restore CONFIG_ZMK_IDLE_SLEEP_TIMEOUT (ZMK's own default is 900000).
 *  [ ] Reconsider CONFIG_BT_PERIPHERAL_PREF_LATENCY=0. It is the single
 *      biggest battery cost in this build: at 0 the radio listens on every
 *      connection event instead of napping. ZMK's default of 30 is fine for a
 *      keyboard that mostly sits idle; 0 is for someone who cares about
 *      response more than runtime.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/endpoints.h>
#include <zmk/hid.h>
#include <zmk_rgbeffect/bringup.h>
#include <zmk_rgbeffect/led_pixel.h>

#if defined(CONFIG_ZMK_RGB_PLAYER_BRINGUP)

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

/* ================================================================== */
/* LAYER 1 - kscan position events (the physical layer)               */
/* ================================================================== */

static volatile uint32_t n_l1;             /* L1 events seen            */
static volatile uint32_t n_l2;             /* L2 callbacks seen         */
static volatile uint32_t n_l3;             /* L3: 0 -> non-empty hops   */
static volatile uint32_t n_l3_empty;       /* L3: non-empty -> 0 hops   */
static volatile uint32_t l3_last_usage;    /* last usage ID in report   */
static volatile uint8_t  l3_last_count;    /* non-modifier keys in last */
static volatile int32_t  l1_last_pos = -1; /* last kscan position       */
static volatile int32_t  l2_last_idx = -1; /* last behaviour key index  */
static volatile bool     l1_pending;       /* latched for the LED read-out */
static volatile bool     l2_pending;
static volatile bool     l3_pending;

/* ------------------------------------------------------------------ */
/* LAYER 3 - the HID report itself (proves what is actually sent)      */
/* ------------------------------------------------------------------ */
/* The single-shot "+50 ms sample" is GONE, and with it the only way this probe
 * could lie. That design could only ever produce a FALSE "empty": if press and
 * release both landed before the sample fired - a quick tap, or a kscan
 * debounce that merged them - the report had already been cleared and the
 * read-out blamed the wrong layer entirely.
 *
 * Instead the LED thread, which is already off the system workqueue and
 * already ticks every 20 ms, POLLS the report across the whole collection
 * window and latches a sticky "I saw a non-empty report" flag. Whichever 20 ms
 * window the report happened to be non-empty in, it is caught. n_l3 counts
 * empty -> non-empty TRANSITIONS so a two second hold does not inflate it.
 * No work item and no timing assumption are left in the probe. */
static void l3_sample(void) {
    static bool prev_nonempty;
    struct zmk_hid_keyboard_report *rep = zmk_hid_get_keyboard_report();
    uint8_t count = 0;
    uint8_t last = 0;

    for (int i = 0; i < CONFIG_ZMK_HID_KEYBOARD_REPORT_SIZE; i++) {
        uint8_t k = rep->body.keys[i];
        if (k != 0) {
            count++;
            last = k;
        }
    }

    bool nonempty = (count > 0);

    if (nonempty && !prev_nonempty) {
        n_l3++;
        LOG_INF("BRINGUP(KPTEST): HID report went NON-EMPTY - %u key(s), last usage 0x%02X",
                (unsigned)count, (unsigned)last);
    } else if (!nonempty && prev_nonempty) {
        n_l3_empty++;
    }

    if (nonempty) {
        l3_pending = true;              /* sticky for this press window */
        l3_last_usage = (uint32_t)last;
        l3_last_count = count;
    }

    prev_nonempty = nonempty;
}

/* Kept so behaviour_kp_with_effect.c still links. The keymap no longer uses
 * &kp_we, and the LED thread polls the report itself, so this is a no-op. */
void bringup_l3_signal(void) {}

/* Called from the kscan context: do no work here, only latch. The strip is
 * 400 ms behind and the blue LED consumes the latches on its own 20 ms tick. */
static int bu_position_handler(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (ev->state) {
        n_l1++;
        l1_last_pos = (int32_t)ev->position;
        l1_pending = true;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(bu_l1_listener, bu_position_handler);
ZMK_SUBSCRIPTION(bu_l1_listener, zmk_position_state_changed);

/* ================================================================== */
/* blue LED (P0.15) - own thread, 20 ms tick                          */
/* ================================================================== */

#define BU_LED_TICK_MS        20
#define BU_BOOT_BLINKS         8    /* EIGHT = PAIRFIX. LATFIX used SEVEN,
                                     * KPTEST SIX, LAYERS / HIDCHK FIVE - the
                                     * boot-blink count is the only reliable way
                                     * to tell which build is on the board. */
#define BU_BOOT_TICKS         (BU_BOOT_BLINKS * 6 + 2)
#define BU_HB_PERIOD_TICKS    50    /* 1 s                              */
#define BU_HB_ON_TICKS         4    /* 80 ms                            */
#define BU_WAIT_PERIOD_TICKS 100    /* 2 s, not-paired double blip      */
#define BU_WAIT_GAP_TICKS     12

#define BU_BLINK_ON_TICKS      3    /* 60 ms lit                        */
#define BU_BLINK_GAP_TICKS     3    /* 60 ms dark between blinks        */
#define BU_COLLECT_TICKS      15    /* 300 ms: lets kscan land, and gives the
                                     * 20 ms report poll every chance to catch a
                                     * window in which the report was non-empty */
#define BU_TAIL_TICKS         10    /* 200 ms before returning to idle  */

enum bu_evt_phase {
    E_IDLE = 0,
    E_COLLECT,
    E_BLINK_ON,
    E_BLINK_OFF,
    E_TAIL,
};

static const struct gpio_dt_spec blue_led = GPIO_DT_SPEC_GET(DT_NODELABEL(blue_led), gpios);
static bool blue_ok;
static bool armed;
static bool ble_was_connected;

K_THREAD_STACK_DEFINE(bu_led_stack, 1024);
static struct k_thread bu_led_thread;

static void led_tick(void) {
    static uint32_t ticks;
    static enum bu_evt_phase phase;
    static uint8_t phase_t;
    static uint8_t blinks_left;

    ticks++;

    bool on = false;

    switch (phase) {
    case E_IDLE:
        if (l1_pending || l2_pending || l3_pending) {
            /* A press arrived. Give the later layers a moment to report too:
             * kscan raises its event before the HID report is written, and the
             * report is only sampled on the LED thread's own tick. Latching
             * immediately would always undercount. */
            phase = E_COLLECT;
            phase_t = 0;
        } else {
            bool connected = zmk_ble_active_profile_is_connected();
            if (connected != ble_was_connected) {
                ble_was_connected = connected;
                LOG_INF("BRINGUP(KPTEST): bluetooth %s",
                        connected ? "CONNECTED" : "not connected");
            }

            if (ticks < BU_BOOT_TICKS) {
                on = ((ticks % 6) < 3);                       /* boot signature */
            } else if (connected) {
                on = ((ticks % BU_HB_PERIOD_TICKS) < BU_HB_ON_TICKS);
            } else {
                /* Not connected - and there are TWO different reasons for that,
                 * which used to be indistinguishable here because only
                 * is_connected() was ever consulted:
                 *
                 *   profile OPEN  -> zmk_ble_profile_is_open() is true, so
                 *                    auth_pairing_accept() returns
                 *                    BT_SECURITY_ERR_SUCCESS. Pairing works.
                 *   profile TAKEN -> auth_pairing_accept() returns
                 *                    BT_SECURITY_ERR_PAIR_NOT_ALLOWED and the
                 *                    host reports "cannot pair with device".
                 *                    That refusal comes from the KEYBOARD, so
                 *                    nothing done on the host side can fix it.
                 *
                 * The blip count now says which state the board is really in:
                 *   2 blips = open,  pairable (what pairing needs)
                 *   3 blips = taken, pairing REFUSED by ZMK                     */
                bool prof_open = zmk_ble_active_profile_is_open();
                uint8_t blips = prof_open ? 2 : 3;
                uint32_t p = ticks % BU_WAIT_PERIOD_TICKS;

                on = false;
                for (uint8_t i = 0; i < blips; i++) {
                    uint32_t start = i * BU_WAIT_GAP_TICKS;
                    if (p >= start && p < start + BU_HB_ON_TICKS) {
                        on = true;
                    }
                }
            }
        }
        break;

    case E_COLLECT:
        if (++phase_t >= BU_COLLECT_TICKS) {
            uint8_t l1 = l1_pending ? 1 : 0;
            uint8_t l2 = l2_pending ? 1 : 0;   /* logged only, not counted */
            uint8_t l3 = l3_pending ? 1 : 0;

            /* KPTEST read-out: TWO layers only, so each count is unambiguous -
             * there is no "which combination was that" left to argue about.
             *   2 = kscan AND a non-empty HID report -> firmware complete
             *   1 = kscan only -> the report never filled
             *   0 = kscan never fired -> physical layer                      */
            blinks_left = (uint8_t)(l1 + l3);
            LOG_INF("BRINGUP(KPTEST): press -> L1(kscan)=%u L3(HIDreport)=%u "
                    "(L2 logged only: %u) [totals L1=%u L2=%u L3trans=%u "
                    "L3empty=%u | last pos=%d idx=%d usage=0x%02X nkeys=%u]",
                    (unsigned)l1, (unsigned)l3, (unsigned)l2,
                    (unsigned)n_l1, (unsigned)n_l2, (unsigned)n_l3,
                    (unsigned)n_l3_empty,
                    (int)l1_last_pos, (int)l2_last_idx,
                    (unsigned)l3_last_usage, (unsigned)l3_last_count);

            l1_pending = false;
            l2_pending = false;
            l3_pending = false;
            phase_t = 0;
            phase = (blinks_left > 0) ? E_BLINK_ON : E_IDLE;
        }
        break;

    case E_BLINK_ON:
        on = true;
        if (++phase_t >= BU_BLINK_ON_TICKS) {
            phase_t = 0;
            phase = E_BLINK_OFF;
        }
        break;

    case E_BLINK_OFF:
        on = false;
        if (++phase_t >= BU_BLINK_GAP_TICKS) {
            phase_t = 0;
            blinks_left--;
            phase = (blinks_left > 0) ? E_BLINK_ON : E_TAIL;
        }
        break;

    case E_TAIL:
        on = false;
        if (++phase_t >= BU_TAIL_TICKS) {
            phase_t = 0;
            phase = E_IDLE;
        }
        break;

    default:
        phase = E_IDLE;
        break;
    }

    if (blue_ok) {
        gpio_pin_set_dt(&blue_led, on ? 1 : 0);
    }
}

/* NOTE ON RECOVERY (PAIRFIX)
 *
 * The fix for "the host cannot pair with this device" is the U+M+O+. combo,
 * which now calls zmk_ble_clear_all_bonds() and therefore resets ZMK's profile
 * table as well as Zephyr's bond store. Press it once after flashing; the blue
 * LED confirms the result (3 blinks -> 2 blinks).
 *
 * There is deliberately NO automatic boot-time repair here. The obvious
 * implementation wants to detect the broken state with
 *
 *     profiles[active].peer != BT_ADDR_LE_ANY  &&  !bt_addr_le_is_bonded(...)
 *
 * and bt_addr_le_is_bonded() is NOT declared by any header this file can reach
 * - bluetooth/bluetooth.h, bluetooth/conn.h and bluetooth/addr.h were all
 * checked against the exact Zephyr the build uses (zmkfirmware/zephyr
 * v3.5.0+zmk-fixes). Without a declaration the compiler assumes it returns int
 * when it really returns bool, which is undefined behaviour. That was caught as
 * a -Wimplicit-function-declaration warning in CI; it is not worth shipping to
 * save one key press.
 */

static void led_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    for (;;) {
        l3_sample();     /* poll the live HID report every 20 ms - sticky, so
                          * no press/release timing can hide a non-empty report */
        led_tick();
        k_sleep(K_MSEC(BU_LED_TICK_MS));
    }
}

/* ================================================================== */
/* WS2812 strip - own thread, 400 ms tick                             */
/* ================================================================== */
/* LATFIX: SWITCHED OFF (BU_ENABLE_STRIP = 0). Two reasons:
 *
 *  1. The strip CANNOT light on this hardware. The schematic's net legend
 *     aliases 3V3 <-> VDD, so all 15 WS2812B are fed from the gated 3.3 V rail
 *     - below the chip's 3.5 V spec floor. That is a PCB rework item, not a
 *     firmware one; no amount of code makes these pixels emit.
 *  2. It is NOT free while dark. One frame is 15 pixels x 24 bits x 8 SPI
 *     bytes = 2880 bytes, which at 3.2 MHz is a ~7.2 ms EasyDMA burst on
 *     SPIM3 every 400 ms. EasyDMA holds the AHB bus and competes with the
 *     radio's timeslots, and this thread (priority 7) sat ABOVE Zephyr's BT
 *     host RX thread (CONFIG_BT_RX_PRIO 8). Spending bus time and thread
 *     priority to push bits into pixels that cannot light is pure waste on a
 *     build whose only remaining goal is input latency.
 *
 * Set BU_ENABLE_STRIP back to 1 to restore the full self-verifying pattern
 * (dark -> red -> green -> blue -> checker, plus the kscan-driven white marker
 * that proves the physical matrix). Worth doing once the rail is reworked to
 * 5 V. */
#define BU_ENABLE_STRIP     0

#if BU_ENABLE_STRIP

#define BU_TICK_MS          400
#define BU_POS_HOLD_TICKS     3     /* 1.2 s marker                     */

enum bu_phase {
    P_DARK = 0,
    P_RED,
    P_GREEN,
    P_BLUE,
    P_CHECKER,
    P_COUNT
};

struct bu_step {
    uint8_t  phase;
    uint16_t ticks;
};

static const struct bu_step bu_plan[] = {
    { P_DARK,    8 },   /* 3.2 s - metering window, zero LED load */
    { P_RED,    15 },   /* 6 s                                    */
    { P_GREEN,  15 },
    { P_BLUE,   15 },
    { P_CHECKER,15 },   /* even RED / odd GREEN                   */
};
#define BU_PLAN_LEN (sizeof(bu_plan) / sizeof(bu_plan[0]))

static volatile uint8_t pos_hold[LED_PIXEL_COUNT];

K_THREAD_STACK_DEFINE(bu_strip_stack, 1024);
static struct k_thread bu_strip_thread;

static void bu_render(uint8_t phase, uint16_t tick) {
    ARG_UNUSED(tick);

    for (uint8_t i = 0; i < LED_PIXEL_COUNT; i++) {
        uint8_t r = 0, g = 0, b = 0;

        switch (phase) {
        case P_RED:
            r = 255;
            break;
        case P_GREEN:
            g = 255;
            break;
        case P_BLUE:
            b = 255;
            break;
        case P_CHECKER:
            if (i & 1) {
                g = 255;
            } else {
                r = 255;
            }
            break;
        case P_DARK:
        default:
            break;
        }

        /* The kscan marker wins over the pattern: it is driven by L1, so it
         * appears even if the keymap / &kp_we is broken. */
        if (pos_hold[i] > 0) {
            r = g = b = 255;
            pos_hold[i]--;
        }

        led_pixel_set(i, r, g, b);
    }
    led_pixel_update();
}

static void strip_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    /* Re-bind here too: SYS_INIT(APPLICATION, 90) runs after POST_KERNEL, so
     * this is normally a no-op, but it removes any init-order doubt. */
    (void)led_pixel_init();

    uint8_t  plan_idx = 0;
    uint16_t tick     = 0;
    int32_t  seen     = -1;

    for (;;) {
        int32_t p = l1_last_pos;
        if (p != seen) {
            seen = p;
            if (p >= 0 && p < LED_PIXEL_COUNT) {
                pos_hold[p] = BU_POS_HOLD_TICKS;
                LOG_INF("BRINGUP(KPTEST): strip marker -> pixel %d", (int)p);
            }
        }

        bu_render(bu_plan[plan_idx].phase, tick);
        tick++;

        if (tick >= bu_plan[plan_idx].ticks) {
            tick = 0;
            plan_idx++;
            if (plan_idx >= BU_PLAN_LEN) {
                plan_idx = 1;   /* repeat from RED; dark window is once */
            }
        }

        k_sleep(K_MSEC(BU_TICK_MS));
    }
}

#endif /* BU_ENABLE_STRIP */

/* ================================================================== */
/* entry points                                                       */
/* ================================================================== */

void bringup_init(void) {
    if (gpio_is_ready_dt(&blue_led)) {
        gpio_pin_configure_dt(&blue_led, GPIO_OUTPUT_INACTIVE);
        blue_ok = true;
    } else {
        LOG_WRN("BRINGUP: on-board blue LED (P0.15) not ready - read-out disabled");
    }

    armed = true;

    /* Both threads are off the system workqueue: ZMK runs the matrix scan on
     * that workqueue, so anything we put there could starve it (and anything
     * blocking there would make the read-out lie).
     *
     * PRIORITY 10, not 6 (LATFIX). The blue-LED probe is now deliberately the
     * lowest-priority thing on the board that still runs, because the resource
     * that must never be delayed is the Bluetooth link:
     *   ZMK's BLE notify thread = 5    (CONFIG_ZMK_BLE_THREAD_PRIORITY)
     *   Zephyr's BT host RX     = 8    (CONFIG_BT_RX_PRIO)
     *   this probe thread       = 10   <- below both
     *   Zephyr's idle thread    = 15
     * At 6 the probe outranked the BT host RX thread and preempted it every
     * 20 ms. A GPIO toggle plus a 6-byte report scan costs microseconds, so
     * nothing is lost - and 10 is still far above idle, so the heartbeat and
     * the blink timing stay accurate. */
    k_thread_create(&bu_led_thread, bu_led_stack, K_THREAD_STACK_SIZEOF(bu_led_stack),
                    led_thread, NULL, NULL, NULL, 10, 0, K_NO_WAIT);
    k_thread_name_set(&bu_led_thread, "bu_led");

#if BU_ENABLE_STRIP
    k_thread_create(&bu_strip_thread, bu_strip_stack, K_THREAD_STACK_SIZEOF(bu_strip_stack),
                    strip_thread, NULL, NULL, NULL, 7, 0, K_NO_WAIT);
    k_thread_name_set(&bu_strip_thread, "bu_strip");
#endif

    LOG_INF("BRINGUP(PAIRFIX): blue LED read-out - on a key press: 2 = kscan AND "
            "non-empty HID report (firmware complete), 1 = kscan only, 0 = matrix "
            "dark. While idle: 1 Hz = connected, 2 blips every 2 s = active "
            "profile OPEN (ZMK accepts pairing), 3 blips every 2 s = profile "
            "TAKEN (ZMK REFUSES pairing with PAIR_NOT_ALLOWED). "
            "Boot signature is EIGHT blinks.");
}

void bringup_key_event(int8_t key_index, bool pressed) {
    if (!armed || key_index < 0 || key_index >= LED_PIXEL_COUNT) {
        return;
    }
    if (pressed) {
        n_l2++;
        l2_last_idx = key_index;
        l2_pending = true;
    }
}

#else /* !CONFIG_ZMK_RGB_PLAYER_BRINGUP */

void bringup_init(void) {}
void bringup_key_event(int8_t key_index, bool pressed) {
    ARG_UNUSED(key_index);
    ARG_UNUSED(pressed);
}
void bringup_l3_signal(void) {}

#endif /* CONFIG_ZMK_RGB_PLAYER_BRINGUP */
