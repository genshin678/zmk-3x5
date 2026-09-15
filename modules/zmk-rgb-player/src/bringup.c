/*
 * bringup.c - LAYERS build: turn "no key output" into one of three hard facts.
 *
 * WHY THIS BUILD EXISTS
 * --------------------
 * Board state: the gated 3.3V/VCC rail is ON (ext-power polarity fixed) and
 * Bluetooth is visible to the host, but NO key produces output. The strip
 * lights are a separate matter and are now understood: the schematic legend
 * aliases 3V3 <-> VDD, so the 15 WS2812B get the gated 3.3 V rail - below the
 * 3.5 V spec floor - which is a hardware rework item, not a firmware bug.
 *
 * The previous build (BLEFIX) removed the transport variable with
 * CONFIG_ZMK_USB=n, so BLE is always the selected endpoint. That killed the
 * most likely cause, but it could not separate a SOFTWARE failure from a
 * PHYSICAL one, because the only key indicator we had hung off &kp_we itself:
 *
 *   switch --diode--> kscan --> transform --> keymap --> &kp_we --> HID
 *                                                             |
 *                                         effects_on_key_down() --> blue LED
 *
 * If &kp_we never runs, that indicator stays dark - indistinguishable from a
 * dead matrix. THIS BUILD REMOVES THE BLIND SPOT by subscribing to ZMK's own
 * events, which fire BEFORE the keymap is consulted.
 *
 * THREE LAYERS, ONE READ-OUT
 * -------------------------
 *  L1  zmk_position_state_changed  <- raised by the kscan subsystem after the
 *      row/column scan produced a contact. This is THE PHYSICAL LAYER: it
 *      proves switch, diode, PCB trace, kscan pins, scan timing and the
 *      matrix transform all worked. It does NOT involve the keymap.
 *  L2  bringup_key_event()         <- called from &kp_we. THE KEYMAP /
 *      BEHAVIOUR LAYER: proof that the binding resolved and the behaviour
 *      actually executed.
 *  L3  (not instrumented)          <- zmk_hid_keyboard_press() +
 *      zmk_endpoints_send_report() inside &kp_we. The transport question is
 *      handled by the CONFIG_ZMK_USB=n change.
 *
 * Note deliberately NOT used as a layer: zmk_keycode_state_changed. ZMK's
 * stock &kp funnels through raise_zmk_keycode_state_changed_from_encoded(),
 * but &kp_we calls zmk_hid_keyboard_press() directly and therefore raises no
 * keycode event at all. Keying the read-out off that event would report a
 * false "no behaviour ran" on a perfectly healthy board.
 *
 * ON-BOARD BLUE LED (P0.15) - its OWN thread, so a blocked SPI write can
 * neither starve it nor fake its timing. On each key press it blinks ONCE PER
 * LAYER THAT REPORTED:
 *
 *   boot              : FIVE quick blinks     <- this build's signature
 *   idle, connected   : clean 1 Hz heartbeat  <- BLE link is up
 *   idle, not paired  : double-blip every 2 s <- advertising; the host has not
 *                                                paired, so keystrokes have
 *                                                nowhere to go
 *   key press, 2 blinks : kscan AND &kp_we both ran -> the firmware input path
 *                         is complete end-to-end; look at host / transport
 *   key press, 1 blink  : kscan ran but &kp_we did NOT -> fault is in the
 *                         keymap / binding / behaviour layer
 *   key press, 0 blinks : kscan never fired -> PHYSICAL layer: switch, diode,
 *                         PCB trace, or a kscan pin
 *
 * WS2812 STRIP - own thread. Same self-describing static pattern as the
 * PATTERN build, PLUS a marker driven straight off the kscan event (so it
 * works even when the keymap is broken):
 *   dark 3.2 s -> red 6 s -> green 6 s -> blue 6 s -> checker 6 s -> loop
 *   any kscan position event -> that pixel goes WHITE for 1.2 s
 *
 * TEMPORARY diagnostic build. Before shipping: delete
 * CONFIG_ZMK_RGB_PLAYER_BRINGUP, restore CONFIG_ZMK_USB=y /
 * CONFIG_USB_DEVICE_STACK=y if USB is wanted, and restore
 * CONFIG_ZMK_IDLE_SLEEP_TIMEOUT=600000.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk_rgbeffect/bringup.h>
#include <zmk_rgbeffect/led_pixel.h>

#if defined(CONFIG_ZMK_RGB_PLAYER_BRINGUP)

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

/* ================================================================== */
/* LAYER 1 - kscan position events (the physical layer)               */
/* ================================================================== */

static volatile uint32_t n_l1;             /* L1 events seen            */
static volatile uint32_t n_l2;             /* L2 callbacks seen         */
static volatile int32_t  l1_last_pos = -1; /* last kscan position       */
static volatile int32_t  l2_last_idx = -1; /* last behaviour key index  */
static volatile bool     l1_pending;       /* latched for the LED read-out */
static volatile bool     l2_pending;

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
#define BU_BOOT_BLINKS         5    /* five = this build                */
#define BU_BOOT_TICKS         (BU_BOOT_BLINKS * 6 + 2)
#define BU_HB_PERIOD_TICKS    50    /* 1 s                              */
#define BU_HB_ON_TICKS         4    /* 80 ms                            */
#define BU_WAIT_PERIOD_TICKS 100    /* 2 s, not-paired double blip      */
#define BU_WAIT_GAP_TICKS     12

#define BU_BLINK_ON_TICKS      3    /* 60 ms lit                        */
#define BU_BLINK_GAP_TICKS     3    /* 60 ms dark between blinks        */
#define BU_COLLECT_TICKS       3    /* 60 ms to let both layers report  */
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
        if (l1_pending || l2_pending) {
            /* A press arrived. Give the second layer a moment to report too:
             * kscan raises its event before the behaviour runs, so latching
             * immediately would always undercount by one. */
            phase = E_COLLECT;
            phase_t = 0;
        } else {
            bool connected = zmk_ble_active_profile_is_connected();
            if (connected != ble_was_connected) {
                ble_was_connected = connected;
                LOG_INF("BRINGUP(LAYERS): bluetooth %s",
                        connected ? "CONNECTED" : "not connected");
            }

            if (ticks < BU_BOOT_TICKS) {
                on = ((ticks % 6) < 3);                       /* boot signature */
            } else if (connected) {
                on = ((ticks % BU_HB_PERIOD_TICKS) < BU_HB_ON_TICKS);
            } else {
                uint32_t p = ticks % BU_WAIT_PERIOD_TICKS;
                on = (p < BU_HB_ON_TICKS) ||
                     (p >= BU_WAIT_GAP_TICKS && p < BU_WAIT_GAP_TICKS + BU_HB_ON_TICKS);
            }
        }
        break;

    case E_COLLECT:
        if (++phase_t >= BU_COLLECT_TICKS) {
            uint8_t l1 = l1_pending ? 1 : 0;
            uint8_t l2 = l2_pending ? 1 : 0;

            blinks_left = (uint8_t)(l1 + l2);
            LOG_INF("BRINGUP(LAYERS): press -> L1(kscan)=%u L2(behaviour)=%u "
                    "[totals L1=%u L2=%u, last pos=%d idx=%d]",
                    (unsigned)l1, (unsigned)l2, (unsigned)n_l1, (unsigned)n_l2,
                    (int)l1_last_pos, (int)l2_last_idx);

            l1_pending = false;
            l2_pending = false;
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

static void led_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    for (;;) {
        led_tick();
        k_sleep(K_MSEC(BU_LED_TICK_MS));
    }
}

/* ================================================================== */
/* WS2812 strip - own thread, 400 ms tick                             */
/* ================================================================== */

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
                LOG_INF("BRINGUP(LAYERS): strip marker -> pixel %d", (int)p);
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
     * blocking there would make the read-out lie). */
    k_thread_create(&bu_led_thread, bu_led_stack, K_THREAD_STACK_SIZEOF(bu_led_stack),
                    led_thread, NULL, NULL, NULL, 6, 0, K_NO_WAIT);
    k_thread_name_set(&bu_led_thread, "bu_led");

    k_thread_create(&bu_strip_thread, bu_strip_stack, K_THREAD_STACK_SIZEOF(bu_strip_stack),
                    strip_thread, NULL, NULL, NULL, 7, 0, K_NO_WAIT);
    k_thread_name_set(&bu_strip_thread, "bu_strip");

    LOG_INF("BRINGUP(LAYERS): blue LED blinks once per layer that reported - "
            "2 = kscan+behaviour, 1 = kscan only, 0 = matrix dark");
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

#endif /* CONFIG_ZMK_RGB_PLAYER_BRINGUP */
