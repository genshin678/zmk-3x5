/*
 * bringup.c - PROBE build: turn the on-board blue LED into the primary
 * instrument, and make the WS2812 path unable to starve the input pipeline.
 *
 * WHY THIS REVISION EXISTS
 * ------------------------
 * Board state after the ext-power fix: 3.3 V rail is ON, BLE works, but the
 * strip is dark and NO key produces output. Everything the firmware *can*
 * configure was verified against the CI artifact (devicetree + .config):
 *   - EXT_POWER  control-gpios = <&gpio0 0xd 0x0>   (ACTIVE_HIGH = rail ON)
 *   - kscan      rows = pro_micro D0/D1/D2, cols = D3/D4/D5/D6/D7
 *                diode-direction = "row2col"
 *   - led_strip  worldsemi,ws2812-spi on spi3, 15 px, 0x70/0x40 @ 4 MHz
 *   - CONFIG_WS2812_STRIP_SPI=y, SPI_NRFX_SPIM=y, LED_STRIP=y
 * so the remaining explanations are (a) a hardware/data-path problem on the
 * strip, and/or (b) an input pipeline that never runs.
 *
 * THE ONE THING THAT COULD KILL *BOTH*
 * ------------------------------------
 * In the previous revision the WS2812 animation ran on the SYSTEM WORKQUEUE
 * every 20 ms. ZMK drives the keyboard matrix scan from that same workqueue.
 * If led_strip_update_rgb() -> spi_write() ever fails to complete, each call
 * blocks until the driver's completion timeout and the workqueue becomes
 * permanently oversubscribed: the matrix is never polled (=> no keys, at all),
 * while BLE - which has its own threads - keeps advertising. Strip dark + dead
 * keys + working Bluetooth is exactly that signature.
 *
 * So this build splits the two:
 *   - WS2812 chase  -> its OWN thread (1 Hz-class, 400 ms/pixel). It can block
 *                      as long as it likes without touching input.
 *   - Blue LED      -> stays on the SYSTEM WORKQUEUE, deliberately, because it
 *                      is now a health *probe* for that queue.
 *
 * BLUE LED (P0.15, independent of the WS2812 chain and of the level shifter)
 * -------------------------------------------------------------------------
 *   boot      : three quick blinks (this build's signature)
 *   steady    : 1 Hz heartbeat, 100 ms on / 900 ms off
 *   key event : SOLID ON for ~600 ms, overrides the heartbeat
 *
 * READING IT
 *   clean 1 Hz  + solid on key press -> workqueue fine AND the whole input
 *                                       path (matrix -> diode -> transform ->
 *                                       keymap -> &kp_we) works.
 *   clean 1 Hz  + never solid        -> workqueue fine, but no key event ever
 *                                       reaches the behavior: matrix/wiring.
 *   ~1 blink every 10 s, or dark     -> the system workqueue is being starved
 *                                       => the SPI/led_strip path is the bug.
 *   totally dark, no 3-blink boot    -> this firmware is not running.
 *
 * WS2812 CHASE (own thread)
 *   15 steps of a single bright pixel walking 0 -> 14 (400 ms each), then two
 *   all-on pulses. Key presses stamp that key's pixel full white.
 *   moving dot          -> SPI + P1.04 + TXS0102 + chain all work
 *   all-on pulses visible-> every LED can light
 *   nothing at all      -> failure is before LED 0: SPI3/MOSI, the level
 *                          shifter, the 300R (R2), or the LED supply itself.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zmk_rgbeffect/bringup.h>
#include <zmk_rgbeffect/led_pixel.h>

#if defined(CONFIG_ZMK_RGB_PLAYER_BRINGUP)

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

/* ---- blue LED: system workqueue, never touches SPI ---- */
#define BU_LED_TICK_MS        20
#define BU_BOOT_TICKS         60    /* 1.2 s: three 3-tick blinks */
#define BU_BOOT_ON_TICKS       3
#define BU_HB_PERIOD_TICKS    50    /* 1 s   */
#define BU_HB_ON_TICKS         5    /* 100 ms */
#define BU_KEY_FLASH_TICKS    30    /* 600 ms solid on a key event */

/* ---- WS2812 chase: dedicated thread ---- */
#define BU_STRIP_START_MS   3000    /* dark window: meter the rail at 0 mA */
#define BU_STRIP_STEP_MS     400
#define BU_CHASE_LEVEL       255
#define BU_BLINK_LEVEL        32
#define BU_BLINK_STEPS         4    /* on/off/on/off after the chase */

/* nice!nano on-board blue LED (P0.15). Independent of the WS2812 chain. */
static const struct gpio_dt_spec blue_led = GPIO_DT_SPEC_GET(DT_NODELABEL(blue_led), gpios);
static bool blue_ok;

static volatile uint8_t  key_hold[LED_PIXEL_COUNT];
static volatile uint32_t key_events;
static uint32_t led_ticks;
static bool armed;
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

    bool on;
    if (fresh) {
        on = true;                                        /* key event wins   */
    } else if (led_ticks < BU_BOOT_TICKS) {
        on = ((led_ticks % 6) < BU_BOOT_ON_TICKS);        /* 3 quick blinks   */
    } else {
        on = ((led_ticks % BU_HB_PERIOD_TICKS) < BU_HB_ON_TICKS); /* 1 Hz     */
    }

    if (blue_ok) {
        gpio_pin_set_dt(&blue_led, on ? 1 : 0);
    }
    k_work_schedule(&led_work, K_MSEC(BU_LED_TICK_MS));
}

/* ------------------------------------------------------------------ */
/* WS2812 chase (own thread) - may block without hurting input         */
/* ------------------------------------------------------------------ */
static void strip_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    uint16_t phase = 0;
    const uint16_t cycle = LED_PIXEL_COUNT + BU_BLINK_STEPS;

    for (;;) {
        bool blink = (phase >= LED_PIXEL_COUNT);
        bool blink_on = blink && (((phase - LED_PIXEL_COUNT) % 2) == 0);

        for (uint8_t i = 0; i < LED_PIXEL_COUNT; i++) {
            uint8_t lvl;
            if (key_hold[i] > 0) {
                lvl = 255;                       /* key stamp, always visible */
            } else if (blink) {
                lvl = blink_on ? BU_BLINK_LEVEL : 0;
            } else {
                lvl = (i == phase) ? BU_CHASE_LEVEL : 0;
            }
            led_pixel_set(i, lvl, lvl, lvl);
        }
        led_pixel_update();

        phase++;
        if (phase >= cycle) {
            phase = 0;
        }
        k_sleep(K_MSEC(BU_STRIP_STEP_MS));
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

    /* The strip gets its own thread so a blocking/failing spi_write() can
     * NEVER starve the system workqueue that ZMK uses for the matrix scan. */
    k_thread_create(&bu_strip_thread, bu_strip_stack,
                    K_THREAD_STACK_SIZEOF(bu_strip_stack), strip_thread, NULL, NULL, NULL,
                    7, 0, K_MSEC(BU_STRIP_START_MS));
    k_thread_name_set(&bu_strip_thread, "bu_strip");

    LOG_INF("BRINGUP(PROBE): blue LED 1Hz + 3-blink boot, solid on key; "
            "WS2812 chase on its own thread");
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
