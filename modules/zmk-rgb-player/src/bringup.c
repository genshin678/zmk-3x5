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
 * 1. Boot chain test, ~5 s, needs no key press:
 *      fill  -> all 15 pixels light dim
 *      walk  -> one pixel sweeps 0 -> 14, one step every ~160 ms
 *      allon -> all 15 light again, then off
 *    Reads: every pixel lights  -> chain + data line + level shifter OK.
 *           sweep stops at k-1    -> the chain is open at LED k (DIN/DOUT of k,
 *                                    or LED k mounted rotated 180 deg).
 *           nothing at all lights -> problem is BEFORE LED 0 (P1.04 data,
 *                                    TXS0102 level shifter, 5 V rail, SPI3).
 *
 * 2. Key indicator, needs no host / no pairing / no text field:
 *      press a key -> that key's pixel lights up and fades out over ~1 s.
 *    This exercises the whole input path (matrix -> diodes -> transform ->
 *    keymap -> behavior) and shows the result locally. Walk the keys and you
 *    can also confirm the layout order (Y U I O P / H J K L ; / N M , . /).
 *
 * 3. Heartbeat: the whole strip gives one short dim blink every ~5 s. If you
 *    see it, the firmware is running AND all 15 pixels are reachable.
 *
 * Notes
 * -----
 * - The renderer runs off a 20 ms k_work_delayable on the system workqueue.
 * - A key press only records state; the next tick (<= 20 ms) renders it, so no
 *   SPI traffic happens from the input thread and there is no tearing.
 * - While this build is active, effects_set_active() is inert and
 *   effects_on_key_down/up() are redirected here, so the strip has exactly one
 *   writer and every LED you observe has exactly one meaning.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk_rgbeffect/bringup.h>
#include <zmk_rgbeffect/led_pixel.h>

#if defined(CONFIG_ZMK_RGB_PLAYER_BRINGUP)

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

#define BU_TICK_MS          20
#define BU_FADE_STEP        16     /* 255/16 = 16 ticks = ~320 ms fade tail */
#define BU_HOLD_TICKS       30     /* ~0.6 s at full brightness after press */
#define BU_FILL_LEVEL       48
#define BU_FILL_TICKS       75     /* ~1.5 s  */
#define BU_WALK_LEVEL       200
#define BU_WALK_STEP_TICKS  8      /* ~160 ms per pixel -> ~2.4 s for 15 */
#define BU_ALLON_LEVEL      96
#define BU_ALLON_TICKS      40     /* ~0.8 s  */
#define BU_HEARTBEAT_EVERY  250    /* 250 * 20 ms = 5 s */
#define BU_HEARTBEAT_TICKS  5      /* ~100 ms blink */
#define BU_HEARTBEAT_LEVEL  60

enum bu_phase { BU_FILL, BU_WALK, BU_ALLON, BU_IDLE };

static uint8_t  frame[LED_PIXEL_COUNT];    /* per-pixel grey level 0..255 */
static uint8_t  key_hold[LED_PIXEL_COUNT]; /* ticks left at full brightness */
static uint32_t ticks;
static uint32_t anim_tick;
static uint8_t  walk_pos;
static uint8_t  heartbeat;
static bool     armed;
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

static void bu_tick(struct k_work *work) {
    ARG_UNUSED(work);
    ticks++;

    switch (phase) {
    case BU_FILL:
        if (anim_tick == 0) {
            bu_fill(BU_FILL_LEVEL);
            bu_push();
            LOG_INF("BRINGUP: fill - all 15 pixels should be lit");
        }
        if (++anim_tick >= BU_FILL_TICKS) {
            anim_tick = 0;
            phase = BU_WALK;
            LOG_INF("BRINGUP: walk - watching for the first dark pixel");
        }
        break;

    case BU_WALK:
        bu_fill(0);
        frame[walk_pos] = BU_WALK_LEVEL;
        bu_push();
        if (++anim_tick >= BU_WALK_STEP_TICKS) {
            anim_tick = 0;
            if (++walk_pos >= LED_PIXEL_COUNT) {
                phase = BU_ALLON;
            }
        }
        break;

    case BU_ALLON:
        if (anim_tick == 0) {
            bu_fill(BU_ALLON_LEVEL);
            bu_push();
            LOG_INF("BRINGUP: all-on, then handing over to key indicator");
        }
        if (++anim_tick >= BU_ALLON_TICKS) {
            anim_tick = 0;
            bu_fill(0);
            bu_push();
            phase = BU_IDLE;
            LOG_INF("BRINGUP: press any key - its pixel should light up");
        }
        break;

    default: { /* BU_IDLE */
        bool dirty = (heartbeat > 0);

        if (heartbeat > 0) {
            heartbeat--;
        }

        for (uint8_t i = 0; i < LED_PIXEL_COUNT; i++) {
            if (key_hold[i] > 0) {
                key_hold[i]--;
                frame[i] = 255;
                dirty = true;
            } else if (frame[i] > 0) {
                frame[i] = (frame[i] > BU_FADE_STEP) ? (uint8_t)(frame[i] - BU_FADE_STEP) : 0;
                dirty = true;
            }
            if (heartbeat > 0 && frame[i] < BU_HEARTBEAT_LEVEL) {
                frame[i] = BU_HEARTBEAT_LEVEL;
            }
        }

        if ((ticks % BU_HEARTBEAT_EVERY) == 0) {
            heartbeat = BU_HEARTBEAT_TICKS;
            bu_fill(BU_HEARTBEAT_LEVEL);
            dirty = true;
        }

        if (dirty) {
            bu_push();
        }
        break;
    }
    }

    k_work_schedule(&bu_work, K_MSEC(BU_TICK_MS));
}

void bringup_init(void) {
    k_work_init_delayable(&bu_work, bu_tick);
    phase      = BU_FILL;
    anim_tick  = 0;
    ticks      = 0;
    walk_pos   = 0;
    heartbeat  = 0;
    armed      = true;
    /* Short delay so the led_strip driver (POST_KERNEL) and ZMK are fully up. */
    k_work_schedule(&bu_work, K_MSEC(300));
    LOG_INF("BRINGUP: self test armed (fill -> walk -> all-on -> key indicator)");
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

#endif /* CONFIG_ZMK_RGB_PLAYER_BRINGUP */
