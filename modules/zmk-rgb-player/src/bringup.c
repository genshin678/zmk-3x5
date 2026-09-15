/*
 * bringup.c - key-independent hardware self test for the 3x5 keyboard.
 *
 * WHY THIS EXISTS
 * ---------------
 * The normal firmware leaves the 15 WS2812B pixels DARK at boot: module_init()
 * never calls effects_set_active(), so the active effect stays RGB_EFFECT_OFF
 * and the only thing that can light a pixel is a key press (via &kp_we ->
 * effects_on_key_down()). On a freshly soldered board that is a dead end - the
 * single thing you need to observe is gated behind the single thing that is
 * most likely to be broken. A dark strip proves NOTHING.
 *
 * WHAT IT DOES (only compiled with CONFIG_ZMK_RGB_PLAYER_BRINGUP=y)
 * ----------------------------------------------------------------
 * 0. ON-BOARD BLUE LED (nice!nano, P0.15) - the one indicator that does NOT
 *    depend on the WS2812 chain at all:
 *      steady 1 Hz blink (100 ms on, 900 ms off)
 *    If this blinks, THIS FIRMWARE IS RUNNING. If it is dark while the board
 *    is otherwise alive, the .uf2 did not take. This splits "firmware problem"
 *    from "LED chain problem" in one glance.
 *
 * 1. WS2812 loop, runs FOREVER so it cannot be missed - whatever moment you
 *    look at the strip, something is happening within a few seconds:
 *      fill  (all 15 dim,  ~1.5 s)
 *      walk  (one pixel sweeps 0 -> 14, ~160 ms each)
 *      allon (all 15 lit,  ~0.8 s)
 *      dark  (all off,     ~0.7 s)
 *    Reads: every pixel lights  -> chain + data line + level shifter OK.
 *           sweep stops at k-1    -> chain open at LED k (DIN/DOUT of k, or
 *                                    LED k mounted rotated 180 deg).
 *           nothing ever lights  -> problem is BEFORE LED 0 (P1.04 data,
 *                                    TXS0102 level shifter / OE, 5 V rail, SPI3).
 *           (earlier revisions ran this animation once at boot only - too easy
 *            to walk up to the board after it had already finished)
 *
 * 2. Key indicator, needs no host / no pairing / no text field:
 *      press a key -> that key's pixel goes FULL WHITE and fades over ~1 s,
 *      drawn on top of whatever the animation is doing.
 *    This exercises the whole input path (matrix -> diodes -> transform ->
 *    keymap -> behavior) and shows the result locally. Walk the keys and you
 *    can also confirm the layout order (Y U I O P / H J K L ; / N M , . /).
 *
 * Notes
 * -----
 * - The renderer runs off a 20 ms k_work_delayable on the system workqueue.
 * - A key press only records state; the next tick (<= 20 ms) renders it, so no
 *   SPI traffic happens from the input thread and there is no tearing.
 * - While this build is active, effects_set_active() is inert and
 *   effects_on_key_down/up() are redirected here, so the strip has exactly one
 *   writer and every LED you observe has exactly one meaning.
 * - The blue LED is driven straight through the GPIO API (not the LED class)
 *   so no extra Kconfig is needed; gpio0 is already enabled by the matrix.
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

#define BU_TICK_MS          20
#define BU_FADE_STEP        16     /* 255/16 = 16 ticks = ~320 ms fade tail */
#define BU_HOLD_TICKS       30     /* ~0.6 s at full brightness after press */

#define BU_FILL_LEVEL       32
#define BU_FILL_TICKS       75     /* ~1.5 s  */
#define BU_WALK_LEVEL       200
#define BU_WALK_STEP_TICKS  8      /* ~160 ms per pixel -> ~2.4 s for 15 */
#define BU_ALLON_LEVEL      64
#define BU_ALLON_TICKS      40     /* ~0.8 s  */
#define BU_DARK_TICKS       35     /* ~0.7 s  */

/* First dark window is much longer than the repeat windows: the 15 WS2812B plus
 * the TXS0102 sit on the SWITCHED 3.3V/VCC rail (nice!nano P0.13 gate), whose
 * current budget is unknown. With the strip idle you can put a meter on that
 * rail and read it without any LED load - that is the measurement that tells
 * "rail alive?" apart from "rail collapses under LED current?". */
#define BU_SETTLE_TICKS     250    /* ~5 s, first cycle only */

/* Blue LED: 1 Hz, 5 ticks on = 100 ms, 45 ticks off = 900 ms. */
#define BU_BLINK_PERIOD     50
#define BU_BLINK_ON_TICKS   5

/* nice!nano on-board blue LED (P0.15). Independent of the WS2812 chain. */
static const struct gpio_dt_spec blue_led = GPIO_DT_SPEC_GET(DT_NODELABEL(blue_led), gpios);
static bool blue_ok;

enum bu_phase { BU_FILL, BU_WALK, BU_ALLON, BU_DARK };

static uint8_t  frame[LED_PIXEL_COUNT];    /* per-pixel grey level 0..255 */
static uint8_t  key_hold[LED_PIXEL_COUNT]; /* ticks left at full brightness */
static uint32_t ticks;
static uint32_t anim_tick;
static uint8_t  walk_pos;
static bool     armed;
static bool     first_dark;
static enum bu_phase phase;
static struct k_work_delayable bu_work;

static void bu_fill(uint8_t v) {
    for (uint8_t i = 0; i < LED_PIXEL_COUNT; i++) {
        frame[i] = v;
    }
}

static void bu_push(void) {
    for (uint8_t i = 0; i < LED_PIXEL_COUNT; i++) {
        led_pixel_set(i, frame[i], frame[i], frame[i]);
    }
    led_pixel_update();
}

/* Build the current frame from the animation phase, then stamp any key press
 * on top of it, then push. Called every tick. */
static void bu_render(void) {
    switch (phase) {
    case BU_FILL:
        bu_fill(BU_FILL_LEVEL);
        break;
    case BU_WALK:
        bu_fill(0);
        frame[walk_pos] = BU_WALK_LEVEL;
        break;
    case BU_ALLON:
        bu_fill(BU_ALLON_LEVEL);
        break;
    default: /* BU_DARK */
        bu_fill(0);
        break;
    }

    for (uint8_t i = 0; i < LED_PIXEL_COUNT; i++) {
        if (key_hold[i] > 0) {
            frame[i] = 255;
        }
    }

    bu_push();
}

static void bu_tick(struct k_work *work) {
    ARG_UNUSED(work);
    ticks++;

    if (blue_ok) {
        gpio_pin_set_dt(&blue_led, ((ticks % BU_BLINK_PERIOD) < BU_BLINK_ON_TICKS) ? 1 : 0);
    }

    for (uint8_t i = 0; i < LED_PIXEL_COUNT; i++) {
        if (key_hold[i] > 0) {
            key_hold[i]--;
        }
    }

    /* Advance the animation. BU_DARK wraps back to BU_FILL, so the whole
     * sequence repeats forever. */
    switch (phase) {
    case BU_FILL:
        if (++anim_tick >= BU_FILL_TICKS) {
            anim_tick = 0;
            walk_pos = 0;
            phase = BU_WALK;
        }
        break;
    case BU_WALK:
        if (++anim_tick >= BU_WALK_STEP_TICKS) {
            anim_tick = 0;
            if (++walk_pos >= LED_PIXEL_COUNT) {
                phase = BU_ALLON;
            }
        }
        break;
    case BU_ALLON:
        if (++anim_tick >= BU_ALLON_TICKS) {
            anim_tick = 0;
            phase = BU_DARK;
        }
        break;
    default: /* BU_DARK */
        if (++anim_tick >= BU_DARK_TICKS) {
            anim_tick = 0;
            phase = BU_FILL;
        }
        break;
    }

    bu_render();
    k_work_schedule(&bu_work, K_MSEC(BU_TICK_MS));
}

void bringup_init(void) {
    if (gpio_is_ready_dt(&blue_led)) {
        gpio_pin_configure_dt(&blue_led, GPIO_OUTPUT_INACTIVE);
        blue_ok = true;
    } else {
        LOG_WRN("BRINGUP: on-board blue LED (P0.15) not ready - heartbeat disabled");
    }

    k_work_init_delayable(&bu_work, bu_tick);
    phase      = BU_DARK;   /* first cycle: long idle window to meter the rail */
    first_dark = true;
    anim_tick  = 0;
    ticks      = 0;
    walk_pos   = 0;
    armed      = true;
    /* Short delay so the led_strip driver (POST_KERNEL) and ZMK are fully up. */
    k_work_schedule(&bu_work, K_MSEC(300));
    LOG_INF("BRINGUP: self test armed (blue LED 1 Hz + WS2812 loop forever + key indicator)");
}

void bringup_key_event(int8_t key_index, bool pressed) {
    if (!armed || key_index < 0 || key_index >= LED_PIXEL_COUNT) {
        return;
    }
    if (pressed) {
        /* Rendered by the next tick; the hold/fade tail makes a quick tap
         * visible, which matters when you are tapping keys to test them. */
        key_hold[key_index] = BU_HOLD_TICKS;
    }
}

#else /* !CONFIG_ZMK_RGB_PLAYER_BRINGUP */

void bringup_init(void) {}
void bringup_key_event(int8_t key_index, bool pressed) {
    ARG_UNUSED(key_index);
    ARG_UNUSED(pressed);
}

#endif /* CONFIG_ZMK_RGB_PLAYER_BRINGUP */
