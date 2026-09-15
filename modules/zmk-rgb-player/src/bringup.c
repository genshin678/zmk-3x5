/*
 * bringup.c - BLEFIX build: prove where the keystrokes are going.
 *
 * WHY THIS BUILD EXISTS
 * --------------------
 * Board state: the gated 3.3V/VCC rail is ON (ext-power polarity fixed), the
 * WS2812B chain is alive but under-volted, Bluetooth is visible to the host -
 * and NO key produces output.
 *
 * RE-VERIFIED against the CI artifact's final devicetree, not assumed:
 *   pro_micro gpio-map (this really is a nice!nano connector):
 *     D0=P0.08  D1=P0.06  D2=P0.17  D3=P0.20  D4=P0.22  D5=P0.24
 *     D6=P1.00  D7=P0.11  D8=P1.04  D9=P1.06
 *   => kscan rows D0/D1/D2 = {P0.08,P0.06,P0.17} and cols D3..D7 =
 *      {P0.20,P0.22,P0.24,P1.00,P0.11} = EXACTLY the schematic's row/column
 *      nets. The WS2812 data pin is D8/P1.04, i.e. NOT shared with any column.
 *   => diode-direction "row2col" matches the schematic (cathode to the column
 *      bus, anode to the row/switch side). Nothing is reversed.
 *   => uart0 status="disabled", i2c0 status="disabled" - they cannot steal
 *      P0.08/P0.06/P0.17/P0.20 even though their pinctrl states name those pins.
 *   => EXT_POWER control-gpios = <&gpio0 0xd 0x0> (ACTIVE_HIGH = rail ON).
 *   => led_strip: ws2812-spi on spi3, chain-length 15, color-mapping G,R,B,
 *      0x70/0x40; exactly ONE writer (ZMK RGB_UNDERGLOW is absent from .config).
 *   => &kp_we is registered identically to ZMK's stock &kp (BEHAVIOR_DT_INST_
 *      DEFINE with the same trailing level/prio/api arguments) and its two
 *      callbacks call the SAME pair of functions the stock behaviour ends up
 *      calling: zmk_hid_keyboard_press() (which wants the RAW usage id, and
 *      gets it) followed by zmk_endpoints_send_report(HID_USAGE_KEY).
 *
 * So the firmware's input path is provably complete, and the remaining
 * candidate is WHERE the report is delivered. ZMK's endpoints layer keeps a
 * single current_instance and send_keyboard_report() uses it - it does not
 * mirror to both transports. preferred_transport DEFAULTS TO USB, so a board
 * that is on USB power and also bonded over BLE sends every key over USB and
 * the BLE host sees nothing. This build removes that variable: CONFIG_ZMK_USB=n
 * makes is_usb_ready() a constant false, so BLE is always the selected
 * transport. Everything else (kscan, keymap, &kp_we, the strip) is untouched.
 *
 * BLUE LED (P0.15) - the read-out, on the SYSTEM workqueue, independent of the
 * WS2812 chain and of the TXS0102:
 *   boot            : FOUR quick blinks        <- this build's signature
 *   not connected   : double-blip every 2 s    <- BLE is advertising; the host
 *                                                has NOT paired/connected yet,
 *                                                so keystrokes have nowhere to
 *                                                go (the most common cause)
 *   connected       : clean 1 Hz heartbeat     <- BLE link is up
 *   any key press   : SOLID 600 ms             <- matrix -> diode -> transform
 *                                                -> keymap -> &kp_we ALL work;
 *                                                if the host still shows no
 *                                                characters, the fault is
 *                                                downstream of the firmware
 *   dark / random   : the system workqueue is being starved (should not happen
 *                     here - the strip runs on its own thread)
 *
 * WS2812 STRIP - unchanged static, self-describing display, refreshed every
 * 400 ms on its OWN thread, so any wrong pixel is a DECODE error, not a
 * refresh artefact:
 *   dark 3.2 s -> red 8 s -> green 8 s -> blue 8 s -> white(40%) 8 s ->
 *   checker (even red / odd green) 8 s -> single red dot walking 0..14 -> loop
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
#include <zmk_rgbeffect/bringup.h>
#include <zmk_rgbeffect/led_pixel.h>

#if defined(CONFIG_ZMK_RGB_PLAYER_BRINGUP)

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

/* ---- blue LED: system workqueue, never touches SPI ---- */
#define BU_LED_TICK_MS        20
#define BU_BOOT_ON_TICKS       3    /* 3 ticks on / 3 off = one blink */
#define BU_BOOT_BLINKS         4    /* FOUR blinks identify this build */
#define BU_BOOT_TICKS         (BU_BOOT_BLINKS * 6 + 2)
#define BU_HB_PERIOD_TICKS    50    /* 1 s   */
#define BU_HB_ON_TICKS         5    /* 100 ms */
#define BU_KEY_FLASH_TICKS    30    /* 600 ms solid on a key event */
/* not-connected ("advertising") pattern: two short blips every 2 s */
#define BU_WAIT_PERIOD_TICKS 100
#define BU_WAIT_GAP_TICKS     12

/* ---- static pattern: dedicated thread ---- */
#define BU_TICK_MS           400
#define BU_WALK_TICKS_PER_PX   3    /* 1.2 s per pixel */
#define BU_WHITE_LEVEL       100    /* ~40 %: keeps the 3V3 rail honest */

enum bu_phase {
    P_DARK = 0,
    P_RED,
    P_GREEN,
    P_BLUE,
    P_WHITE,
    P_CHECKER,
    P_WALK,
    P_COUNT
};

struct bu_step {
    uint8_t  phase;
    uint16_t ticks;
};

static const struct bu_step bu_plan[] = {
    { P_DARK,    8 },                          /* 3.2 s - meter window      */
    { P_RED,    20 },                          /* 8.0 s                     */
    { P_GREEN,  20 },
    { P_BLUE,   20 },
    { P_WHITE,  20 },
    { P_CHECKER,20 },                          /* even RED / odd GREEN      */
    { P_WALK,   LED_PIXEL_COUNT * BU_WALK_TICKS_PER_PX }, /* 45 t = 18 s    */
};
#define BU_PLAN_LEN (sizeof(bu_plan) / sizeof(bu_plan[0]))

/* nice!nano on-board blue LED (P0.15). Independent of the WS2812 chain. */
static const struct gpio_dt_spec blue_led = GPIO_DT_SPEC_GET(DT_NODELABEL(blue_led), gpios);
static bool blue_ok;

static volatile uint8_t  key_hold[LED_PIXEL_COUNT];
static volatile uint32_t key_events;
static uint32_t led_ticks;
static bool armed;
static bool ble_was_connected;
static struct k_work_delayable led_work;

K_THREAD_STACK_DEFINE(bu_strip_stack, 1024);
static struct k_thread bu_strip_thread;

/* ------------------------------------------------------------------ */
/* blue LED (system workqueue) - doubles as a workqueue health probe   */
/* ------------------------------------------------------------------ */
static void led_tick(struct k_work *work) {
    ARG_UNUSED(work);
    led_ticks++;

    bool fresh = false;
    for (uint8_t i = 0; i < LED_PIXEL_COUNT; i++) {
        if (key_hold[i] > 0) {
            key_hold[i]--;
            fresh = true;
        }
    }

    bool connected = zmk_ble_active_profile_is_connected();
    if (connected != ble_was_connected) {
        ble_was_connected = connected;
        LOG_INF("BRINGUP(BLEFIX): bluetooth %s", connected ? "CONNECTED" : "not connected");
    }

    bool on;
    if (fresh) {
        on = true;                                          /* key event wins */
    } else if (led_ticks < BU_BOOT_TICKS) {
        on = ((led_ticks % 6) < BU_BOOT_ON_TICKS);          /* 4 quick blinks */
    } else if (connected) {
        on = ((led_ticks % BU_HB_PERIOD_TICKS) < BU_HB_ON_TICKS);   /* 1 Hz   */
    } else {
        /* advertising, host has not paired: double-blip every 2 s */
        uint32_t p = led_ticks % BU_WAIT_PERIOD_TICKS;
        on = (p < BU_HB_ON_TICKS) ||
             (p >= BU_WAIT_GAP_TICKS && p < BU_WAIT_GAP_TICKS + BU_HB_ON_TICKS);
    }

    if (blue_ok) {
        gpio_pin_set_dt(&blue_led, on ? 1 : 0);
    }
    k_work_schedule(&led_work, K_MSEC(BU_LED_TICK_MS));
}

/* ------------------------------------------------------------------ */
/* static pattern renderer (own thread)                                */
/* ------------------------------------------------------------------ */
static void bu_render(uint8_t phase, uint16_t tick) {
    uint8_t walk_px = (uint8_t)(tick / BU_WALK_TICKS_PER_PX);

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
        case P_WHITE:
            r = g = b = BU_WHITE_LEVEL;
            break;
        case P_CHECKER:
            if (i & 1) {
                g = 255;              /* odd  -> GREEN */
            } else {
                r = 255;              /* even -> RED   */
            }
            break;
        case P_WALK:
            if (i == walk_px) {
                r = 255;              /* one RED dot, everything else dark */
            }
            break;
        case P_DARK:
        default:
            break;
        }

        /* A key press always wins: that key's own pixel turns WHITE, which
         * also proves which physical key maps to which index. */
        if (key_hold[i] > 0) {
            r = g = b = 255;
        }

        led_pixel_set(i, r, g, b);
    }
    led_pixel_update();
}

static void strip_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    /* Re-bind here as well: SYS_INIT(APPLICATION, 90) runs after POST_KERNEL,
     * so this is normally a no-op, but it removes any init-order doubt. */
    (void)led_pixel_init();

    /* plan[0] is P_DARK, so the very first thing the strip does is go black
     * and stay black for 3.2 s - that is the window for metering the 3V3 rail
     * with zero LED load. After the last phase the plan wraps back to index 1
     * (P_RED), so the dark window happens once per power-up. */
    uint8_t  plan_idx = 0;
    uint16_t tick     = 0;

    for (;;) {
        const struct bu_step *st = &bu_plan[plan_idx];

        if (tick == 0) {
            LOG_INF("BRINGUP(BLEFIX): phase %u (%u ticks)",
                    (unsigned int)st->phase, (unsigned int)st->ticks);
        }

        bu_render(st->phase, tick);
        tick++;

        if (tick >= st->ticks) {
            tick = 0;
            plan_idx++;
            if (plan_idx >= BU_PLAN_LEN) {
                plan_idx = 1;   /* repeat from RED; the dark window is once */
            }
        }

        k_sleep(K_MSEC(BU_TICK_MS));
    }
}

void bringup_init(void) {
    if (gpio_is_ready_dt(&blue_led)) {
        gpio_pin_configure_dt(&blue_led, GPIO_OUTPUT_INACTIVE);
        blue_ok = true;
    } else {
        LOG_WRN("BRINGUP: on-board blue LED (P0.15) not ready - heartbeat disabled");
    }

    k_work_init_delayable(&led_work, led_tick);
    armed = true;
    k_work_schedule(&led_work, K_MSEC(200));

    /* Own thread: a blocking spi_write() can never starve the system
     * workqueue that ZMK uses for the matrix scan. */
    k_thread_create(&bu_strip_thread, bu_strip_stack,
                    K_THREAD_STACK_SIZEOF(bu_strip_stack), strip_thread, NULL, NULL, NULL,
                    7, 0, K_NO_WAIT);
    k_thread_name_set(&bu_strip_thread, "bu_strip");

    LOG_INF("BRINGUP(BLEFIX): USB endpoint DISABLED (pure BLE); blue LED = "
            "4 blinks boot, double-blip = not connected, 1 Hz = connected, "
            "solid 600 ms = key press");
}

void bringup_key_event(int8_t key_index, bool pressed) {
    if (!armed || key_index < 0 || key_index >= LED_PIXEL_COUNT) {
        return;
    }
    if (pressed) {
        key_hold[key_index] = BU_KEY_FLASH_TICKS;
        key_events++;
        LOG_INF("BRINGUP: key down idx=%d (total %u)", key_index, key_events);
    }
}

#else /* !CONFIG_ZMK_RGB_PLAYER_BRINGUP */

void bringup_init(void) {}
void bringup_key_event(int8_t key_index, bool pressed) {
    ARG_UNUSED(key_index);
    ARG_UNUSED(pressed);
}

#endif /* CONFIG_ZMK_RGB_PLAYER_BRINGUP */
