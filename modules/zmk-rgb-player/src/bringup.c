/*
 * bringup.c - PATTERN build: a static, self-describing test display.
 *
 * WHAT CHANGED AND WHY
 * --------------------
 * PROBE build result: the strip is ALIVE (pixels light up, something moves)
 * but the image is wrong - LED 1 green, a few pixels moving, the rest stuck
 * on. That rules out "no data at all" and points at DATA INTEGRITY.
 *
 * Everything the firmware can be blamed for was verified against the CI
 * artifact (devicetree + .config), and it is all correct:
 *   - EXT_POWER  control-gpios = <&gpio0 0xd 0x0>  (ACTIVE_HIGH = rail ON)
 *   - kscan      rows D0/D1/D2, cols D3/D4/D5/D6/D7, diode-direction row2col
 *   - led_strip  spi-one-frame 0x70, spi-zero-frame 0x40, chain-length 15,
 *                color-mapping = GREEN RED BLUE (0x2 0x1 0x3) for WS2812B
 *   - exactly ONE writer: ZMK's built-in underglow is disabled, the effects
 *     engine is fenced off while CONFIG_ZMK_RGB_PLAYER_BRINGUP is set
 *     (effects_set_active / effects_on_key_down return early), so the 10 ms
 *     effects tick never runs and can never interleave frames with this file.
 *
 * THE TIMING MATH (the reason this build also slows the SPI clock down)
 * -------------------------------------------------------------------
 * Zephyr's ws2812_spi driver (v3.5.0) serializes ONE WS2812 bit into ONE
 * FULL SPI BYTE:
 *
 *     ws2812_spi_ser(): buf[i] = color & BIT(7-i) ? one_frame : zero_frame;
 *                       px_buf += 8;   // 8 SPI bytes per colour channel
 *
 * so with spi-one-frame = 0x70 (0111_0000) and spi-zero-frame = 0x40
 * (0100_0000) the pulse widths are 3 and 1 SPI bit:
 *
 *   SPI clock | T1H (3 bits)      | T0H (1 bit)       | WS bit period (8 bits)
 *   4.0 MHz   | 0.75 us  (ok)     | 0.25 us (MIN edge)| 2.0 us
 *   3.2 MHz   | 0.94 us  (ok)     | 0.31 us  (ok)     | 2.5 us
 *   WS2812B   | 0.65 - 0.95 us    | 0.25 - 0.55 us    | >= 0.65 us
 *
 * The data does not go straight from the nRF to the LED: it passes through
 * U1 (TXS0102), a bidirectional translator built for I2C/open-drain that
 * pulls its high level up through a ~10k internal resistor and relies on a
 * one-shot edge accelerator. A 10k pull-up on ~25 pF of line + LED input
 * capacitance spends roughly 100-150 ns just crossing the LED's input
 * threshold (0.7 x VDD). At 4 MHz that eats most of the margin of a "1" bit
 * (0.75 us -> effectively ~0.60-0.65 us, i.e. below the 0.65 us minimum),
 * so a "1" can be decoded as a "0" - which is exactly the kind of corruption
 * that produces wrong colours on some pixels and correct ones on others.
 * At 3.2 MHz the same "1" is still ~0.80 us wide at the LED pin: centre of
 * spec. That is the whole point of this build - it is the cheapest possible
 * test of "is the signal marginal?" (no soldering required).
 *
 * Note: T1H and T0H move in the same direction, so there is no clock that
 * fixes both perfectly. 3.2 MHz is the best compromise: T0H well clear of
 * its minimum and T1H still inside its window.
 *
 * THE DISPLAY (static content, refreshed every 400 ms - nothing churns)
 * --------------------------------------------------------------------
 *   phase        duration   what you should see
 *   dark          3.2 s     all 15 dark          <- meter the 3V3 rail here
 *   red           8.0 s     all 15 RED
 *   green         8.0 s     all 15 GREEN
 *   blue          8.0 s     all 15 BLUE
 *   white         8.0 s     all 15 WHITE (dim, ~40 %, to stay inside the
 *                           current budget of the switched 3V3 rail)
 *   checker       8.0 s     even pixels RED, odd pixels GREEN (index test)
 *   walk         18.0 s     one RED dot walks index 0 -> 14, 1.2 s each,
 *                           everything else dark (chain-topology test)
 *   then it repeats from "red".
 *
 * READING IT
 *   all 15 correct in every phase -> the chain, the GRB order and the rail
 *     are all fine with a slow clock; the old failure was an edge-rate /
 *     timing margin problem. Keep 3.2 MHz (or bypass U1 - see below).
 *   LED 1 green while the rest are red -> the first LED decoded its 3 bytes
 *     one byte early/late: pure signal-integrity at the FIRST pixel, i.e.
 *     U1 / R2 / the 3V3 rail - not the firmware.
 *   LEDs 1..n correct, the rest dark or junk -> the chain or the supply
 *     fails at LED n. With 3V3 (below the WS2812B 3.5 V minimum) this is the
 *     expected signature of a supply droop: move the strip's VDD to VBUS/5 V.
 *   walk dot lands on the wrong pixel or two pixels light together -> the
 *     frame is still misaligned; report the exact offset.
 *   everything dark in every phase -> the data line never reaches LED 1.
 *
 * BLUE LED (P0.15, on the SYSTEM workqueue, independent of the strip)
 *   boot      : three quick blinks (this build's signature)
 *   steady    : 1 Hz heartbeat  (the workqueue health probe)
 *   key event : SOLID ON ~600 ms + that key's own pixel goes WHITE
 *   => "1 Hz + solid on every press" proves matrix -> diode -> transform ->
 *      keymap -> &kp_we is intact and the problem is host-side transport.
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

    uint8_t  plan_idx = (uint8_t)(P_COUNT); /* force a phase change on entry */
    uint16_t tick     = 0;

    for (;;) {
        if (plan_idx >= BU_PLAN_LEN || tick == 0) {
            if (plan_idx >= BU_PLAN_LEN) {
                plan_idx = 1;   /* skip the dark window after the first cycle */
            } else {
                plan_idx++;
            }
            tick = 0;
            LOG_INF("BRINGUP(PATTERN): phase %u (%u ticks)",
                    bu_plan[plan_idx].phase, bu_plan[plan_idx].ticks);
        }

        bu_render(bu_plan[plan_idx].phase, tick);
        tick++;

        if (tick >= bu_plan[plan_idx].ticks) {
            plan_idx++;
            tick = 0;
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

    LOG_INF("BRINGUP(PATTERN): static R/G/B/white/checker/walk @ 400 ms; "
            "blue LED 1 Hz + 3-blink boot, solid on key");
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
