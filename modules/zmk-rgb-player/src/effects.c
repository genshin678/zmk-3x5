/*
 * effects.c - 6 effect routines on the 15-pixel WS2812B strip
 *
 * Effect 0 = OFF (all pixels dark, no tick).
 * Effect 1/2/3 = ZMK built-in solid / breathing / rainbow (ZMK drives them).
 * Effect 4/5/6 = per-pixel effects driven by our own render thread tick.
 *
 * For effect 6 (ripple), the wave origin is the key most recently PRESSED,
 * latched by ripple_trigger(); the ring is a single pulse, not a loop.
 *
 * All 7 effects are rendered by this module directly via the WS2812B
 * driver; none rely on ZMK's built-in rgb_underglow subsystem.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk_rgbeffect/led_pixel.h>
#include <zmk_rgbeffect/rgb_control.h>
#include <zmk_rgbeffect/effects.h>
#include <zmk_rgbeffect/bringup.h>

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

/* Render tick period, in ms.
 *
 * This used to be 10 ms, which is more than the job needs. One frame is
 * 15 pixels x 24 bits x 1 SPI byte = 360 bytes - Zephyr's ws2812_spi driver
 * serialises ONE WS2812 bit into ONE full SPI byte (see the spi-one-frame /
 * spi-zero-frame comment in the shield overlay) - which at the overlay's
 * 8 MHz takes 360 us, plus the 300 us latch gap the newer WS2812B dies need:
 * ~660 us per frame. And it is SYNCHRONOUS: led_pixel_update() ->
 * led_strip_update_rgb() -> spi_write() does not return until the DMA has
 * drained.
 *
 * BACK TO 10 ms (100 fps), from 20 ms (50 fps).
 *
 * The 20 ms figure was chosen when this tick was a k_work_delayable on the
 * SYSTEM WORKQUEUE - the same queue ZMK scans the key matrix on - so every
 * render sat directly in front of input handling, and 12% of that queue was
 * a real price to pay. THAT REASON IS GONE: the tick is now a dedicated
 * thread at priority 12 (see below), which every input path preempts, and
 * the 660 us is DMA time - the thread sleeps, it does not burn CPU. 100 fps
 * therefore costs ~6.6% of ONE low-priority thread and nothing else.
 *
 * What it buys is smoothness where it is actually visible: halving the frame
 * period halves the per-frame step of every animation. That step is what the
 * strip was showing as stutter.
 *
 * CORRECTION, for the record: an earlier revision of bringup.c claimed one
 * frame was 2880 bytes / 7.2 ms. That was wrong by a factor of 8 - it counted
 * the 8 SPI bits inside a frame byte as though each were its own byte. The
 * real figure is 360 bytes. (It is re-enabled in this revision, the hardware
 * now having a working 5 V feed.)
 *
 * Every animation constant below states its timing as a WALL-CLOCK period and
 * converts it to frames through FRAMES(), so this number stays the only knob.
 * That matters: the 10 -> 20 move had to double three separate magic numbers
 * by hand, and one of them (the twinkle decay) would have been missed. */
#define EFFECTS_TICK_MS 10

/* Frames in a wall-clock period: FRAMES(160) is 160 ms expressed in ticks.
 * The effects below describe what you SEE in milliseconds and divide here,
 * so the tick period stays the only thing anyone has to change. */
#define FRAMES(ms) ((ms) / EFFECTS_TICK_MS)

/* ---------------------------------------------------------------- */
/* the render tick: a DEDICATED THREAD, not the system workqueue     */
/* ---------------------------------------------------------------- */
/* This used to be a k_work_delayable - i.e. it ran on the SYSTEM WORKQUEUE,
 * and ZMK runs the key-matrix scan on that same workqueue. Every render is
 * led_pixel_update(): ONE synchronous ~660 us 360-byte SPI burst at 8 MHz (see
 * the EFFECTS_TICK_MS note above), so each frame sat directly in front of the
 * matrix scan and delayed input by up to a frame. At 10 ms that is ~6.6% of
 * the input path's own thread spent pushing pixels.
 *
 * Nobody could notice while the strip was dark: the engine was inert in
 * the bring-up builds, and the latency work had the strip thread switched
 * off. The moment the engine went live again the load became real - which
 * is why "the lights work now" and "input got slower" arrived in the same
 * report.
 *
 * A dedicated thread at a priority BELOW the workqueue makes the ordering
 * permanent: the workqueue outranks it, so a scan that becomes ready preempts
 * a render in progress. Input can never wait on pixels again. (The bring-up
 * read-out used the same reasoning - see the note in bringup.c on why both of
 * its threads were kept off the system workqueue.)
 *
 * Priority 12 sits below every radio path:
 *   ZMK's BLE notify thread = 5    (CONFIG_ZMK_BLE_THREAD_PRIORITY)
 *   Zephyr's BT host RX     = 8    (CONFIG_BT_RX_PRIO)
 *   this render thread      = 12   <- below both of them
 *   Zephyr's idle thread    = 15
 * A 100 fps animation is still the least urgent thing on the board. */
#define EFFECTS_THREAD_PRIORITY 12
/* 1536 was enough while this thread only staged pixels and pushed them.
 * render_ripple() now runs an integer square root and an hsv_to_rgb()
 * conversion inside the same frame as the SPI call, and the true high-water
 * mark of this thread has never been measured on hardware - the shipping
 * build runs with logging off, so nothing reports it. 512 bytes of margin
 * costs 0.3% of the free RAM, which is far cheaper than the silent
 * corruption a stack overflow would cause. Both additions are leaf calls
 * with tiny frames, so this is margin, not a larger requirement. */
#define EFFECTS_STACK_SIZE      2048

K_THREAD_STACK_DEFINE(effects_stack, EFFECTS_STACK_SIZE);
static struct k_thread effects_thread;

static rgb_effect_t active = RGB_EFFECT_OFF;
static int8_t active_key = -1;         /* for SINGLE_KEY effect */
static bool player_takeover = false;
static uint32_t tick_count = 0;
static volatile bool tick_running;

/* Ripple pulse state.
 *
 * These three are written by the KEYPRESS path (input context) and read and
 * advanced by the render thread. Each is a single, naturally aligned word -
 * the M4 cannot tear it - and a store landing mid-frame costs at most ONE
 * 10 ms frame drawn from the previous origin, which is invisible. So no lock.
 *
 * The render thread deliberately never writes ripple_origin: if it retired a
 * finished pulse by clearing the origin itself, a press arriving in the few
 * cycles between its read and that write would be overwritten and silently
 * dropped. Completion is therefore inferred from ripple_r_fp, which only the
 * render thread advances, and a new press simply resets it. */
static volatile int8_t   ripple_origin = -1;    /* key index, -1 = no ring */
static volatile uint16_t ripple_r_fp   = 0;     /* front radius, sub-units */
static volatile bool     ripple_lit    = false; /* strip shows a ring now */

static void effects_render_frame(void);

static void effects_thread_fn(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (tick_running) {
        effects_render_frame();
        k_sleep(K_MSEC(EFFECTS_TICK_MS));
    }
}

static void start_tick(void) {
    if (tick_running) return;
    tick_running = true;
    k_thread_create(&effects_thread, effects_stack,
                    K_THREAD_STACK_SIZEOF(effects_stack),
                    effects_thread_fn, NULL, NULL, NULL,
                    EFFECTS_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&effects_thread, "rgb_eff");
}

static void stop_tick(void) {
    if (!tick_running) return;
    tick_running = false;
    /* Join, so that when this returns the thread is gone and no render can be
     * in flight. render_off() clears the strip immediately after calling this,
     * and a tick that outlived it would repaint one stale frame. The thread
     * notices the flag within EFFECTS_TICK_MS, so 500 ms is generous. */
    (void)k_thread_join(&effects_thread, K_MSEC(500));
}

static struct led_rgb hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v) {
    struct led_rgb c = {0, 0, 0};
    if (s == 0) { c.r = c.g = c.b = v; return c; }
    uint8_t region = h / 60;
    /* Scale the in-region offset (0..59) into 0..255 so the (s * rem) >> 8
     * products below stay inside the 8-bit range.
     *
     * BUG FIXED HERE: this was `* 6`. That factor belongs to the classic
     * 8-bit-hue snippet, where h is 0..255 and region = h / 43 - there the
     * remainder is 0..42 and * 6 lands in 0..252. Here h is 0..359 and
     * region = h / 60, so the remainder is 0..59 and the correct factor is
     * 255 / 60 = 4.25. With * 6 the product reached 354, and rem is a
     * uint8_t, so everything >= 256 wrapped modulo 256. Hue therefore mapped
     * to a scrambled, non-monotonic colour and the rainbow effect looked like
     * random flickering - which is exactly what was reported. */
    uint8_t rem = (uint8_t)(((h - region * 60) * 255) / 60);
    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * rem) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8;
    switch (region) {
        case 0: c.r = v; c.g = t; c.b = p; break;
        case 1: c.r = q; c.g = v; c.b = p; break;
        case 2: c.r = p; c.g = v; c.b = t; break;
        case 3: c.r = p; c.g = q; c.b = v; break;
        case 4: c.r = t; c.g = p; c.b = v; break;
        default:c.r = v; c.g = p; c.b = q; break;
    }
    return c;
}

static void render_off(void) {
    stop_tick();
    led_pixel_clear();
    led_pixel_update();
}

static uint8_t breathing_scale(void) {
    /* ~16 s for a full up-and-down cycle: each of the 100 triangle steps is
     * 160 ms of wall clock, at any tick period. */
    uint32_t t = (tick_count / FRAMES(160)) % 100;
    uint32_t tri = (t < 50) ? (t * 2) : ((100 - t) * 2); /* 0..100 triangle */
    return (uint8_t)(20 + (tri * 80) / 100);             /* 20%..100% */
}

static void render_solid(void) {
    struct led_rgb c = hsv_to_rgb(rgb_control_get_hue(), 100,
                                  rgb_control_get_brightness());
    for (int i = 0; i < LED_PIXEL_COUNT; i++) {
        led_pixel_set((uint8_t)i, c.r, c.g, c.b);
    }
    led_pixel_update();
}

static void render_breathing(void) {
    uint8_t scale = breathing_scale();
    uint8_t v = (uint8_t)((rgb_control_get_brightness() * scale) / 100);
    struct led_rgb c = hsv_to_rgb(rgb_control_get_hue(), 100, v);
    for (int i = 0; i < LED_PIXEL_COUNT; i++) {
        led_pixel_set((uint8_t)i, c.r, c.g, c.b);
    }
    led_pixel_update();
}

/* Hue degrees per frame for a ~1.2 s full sweep: 360 * tick / 1200. */
#define RAINBOW_HUE_STEP (360 * EFFECTS_TICK_MS / 1200)

static void render_rainbow(void) {
    for (int i = 0; i < LED_PIXEL_COUNT; i++) {
        /* A full 360-degree sweep takes ~1.2 s at any tick, so only the time
         * term scales. The +24 per pixel is a SPATIAL offset and does not. */
        uint16_t hue = (uint16_t)((tick_count * RAINBOW_HUE_STEP + i * 24) % 360);
        struct led_rgb c = hsv_to_rgb(hue, 255, rgb_control_get_brightness());
        led_pixel_set((uint8_t)i, c.r, c.g, c.b);
    }
    led_pixel_update();
}

static void render_single_key(void) {
    led_pixel_clear();
    if (active_key >= 0 && active_key < LED_PIXEL_COUNT) {
        struct led_rgb c = hsv_to_rgb(rgb_control_get_hue(), 255,
                                      rgb_control_get_brightness());
        led_pixel_set((uint8_t)active_key, c.r, c.g, c.b);
    }
    led_pixel_update();
}

/* How long a star takes to die, and the per-frame step that produces it. */
#define TWINKLE_FADE_MS     170
#define TWINKLE_DECAY_STEP  ((255 * EFFECTS_TICK_MS) / TWINKLE_FADE_MS)

/* Spawn chance per pixel per frame, out of 256. Stated as a RATE so that the
 * strip does not become twice as busy when the tick shortens: ~14 stars/s
 * across the 15 pixels. With a ~170 ms life that leaves two or three lit at
 * any instant - sparkle rather than static. (It used to be a flat
 * `roll < 20`, i.e. ~58/s, which once the fade below started working would
 * have had most of the strip lit at all times.) */
#define TWINKLE_SPAWN_TH \
    ((256 * 14 * EFFECTS_TICK_MS) / (1000 * LED_PIXEL_COUNT))

static void render_twinkle(void) {
    /* BUG FIXED HERE: decay[] was counted down but never APPLIED. The pixel
     * was painted at the full colour latched when the star was spawned, and
     * then simply went out when the counter reached zero - so a star was a
     * hard on/off square wave lasting 170 ms, which is exactly what reads as
     * flicker instead of glow. The stored colour is now kept at FULL scale
     * and every frame is scaled by decay, so a star rises instantly and dies
     * smoothly. Because the fade is applied at render time, bri is read per
     * frame and an existing star follows a brightness change too. */
    static uint8_t decay[LED_PIXEL_COUNT];
    static uint8_t r[LED_PIXEL_COUNT], g[LED_PIXEL_COUNT], b[LED_PIXEL_COUNT];
    const uint8_t bri = rgb_control_get_brightness();

    for (int i = 0; i < LED_PIXEL_COUNT; i++) {
        if (decay[i] == 0) {
            uint32_t roll = (tick_count * 1103515245u + i * 12345u + 7) & 0xFF;
            if (roll < TWINKLE_SPAWN_TH) {
                uint16_t hue = (rgb_control_get_hue() + i * 24 + roll) % 360;
                struct led_rgb c = hsv_to_rgb(hue, 255, 255); /* full scale */
                r[i] = c.r; g[i] = c.g; b[i] = c.b;
                decay[i] = 255;
            }
        } else {
            decay[i] = (decay[i] > TWINKLE_DECAY_STEP)
                           ? (uint8_t)(decay[i] - TWINKLE_DECAY_STEP) : 0;
        }
        /* decay == 0 with no spawn leaves k at 0, so a dead star is painted
         * black without needing to clear its stored colour. */
        const uint8_t k = (uint8_t)(((uint32_t)bri * decay[i]) / 255);
        led_pixel_set((uint8_t)i, (uint8_t)((r[i] * k) / 255),
                      (uint8_t)((g[i] * k) / 255),
                      (uint8_t)((b[i] * k) / 255));
    }
    led_pixel_update();
}

/*
 * The pixel grid.
 *
 * The 15 pixels are mounted one per key on a 3x5 keypad and chained in KEYMAP
 * order, so pixel index i sits at column i % 5, row i / 5 - the same grid the
 * keymap reads as  Y U I O P / H J K L ; / N M , . /  and the same one the
 * kscan transform in the shield overlay produces.
 *
 * This is the ONE assumption in the ripple that the firmware cannot check for
 * itself. It is verifiable on the bench in thirty seconds without reflashing:
 * switch to effect 4, which lights the single pixel whose index equals the key
 * you are holding - if the lit pixel is under your finger for every key, the
 * grid is right. If it is mirrored or rotated, pixel_xy() is the one place to
 * fix and nothing else changes. */
#define GRID_COLS 5
BUILD_ASSERT(LED_PIXEL_COUNT % GRID_COLS == 0,
             "pixel count must fill the grid exactly");

/*
 * Ripple: ONE expanding ring, emitted by the key you press.
 *
 * This replaces a wave that was wrong in five independent ways at once, all of
 * them visible on the strip:
 *
 *  1. It was a PERMANENT self-running loop - dist_base = (tick / 6) % 29 - not
 *     a response to a press. It swept out from the last key, vanished and
 *     started over, forever. What was asked for is one pulse per press.
 *  2. It was not one colour: every step away from the origin shifted the hue
 *     by 15 degrees, so it was a rainbow streak. It is now the module's
 *     current hue and brightness, so H+; and Y+P still tune it.
 *  3. It was ONE-DIMENSIONAL: it walked the pixel indices up and down, so from
 *     a key in the middle it only ran off left and right. With the grid above,
 *     a genuine ring is available, and the distance below is Euclidean - which
 *     is what makes it spread in all four directions.
 *  4. Its amplitude ramp was broken twice over. `falloff` counted DOWN with
 *     distance from the head, so the peak sat one pixel BEHIND the front and
 *     the front itself was the dimmest part; and at offset -2 the numerator
 *     255 * 4 / 3 = 340 was cast to uint8_t and wrapped to 84, a brightness
 *     with no relation to the one intended. The band is now a symmetric
 *     parabola centred on the front, accumulated in int and divided before the
 *     narrowing cast.
 *  5. The front advanced a WHOLE pixel every 6 frames - 8.3 distinct images
 *     per second inside a 50 fps render - which is the low frame rate that was
 *     reported. Distance is carried in 1/RIPPLE_FP of a key, so the front now
 *     moves 1/8 of a key per frame and the motion is continuous.
 *
 * Timing: the front advances RIPPLE_STEP_FP / RIPPLE_FP of a key per frame,
 * i.e. one key per RIPPLE_MS_PER_KEY. A corner-to-opposite-corner pulse is the
 * longest at 41 frames = 410 ms; from the middle key, whose farthest pixel is
 * only 2.2 keys away, the wave is over in about 230 ms. The duration therefore
 * depends on where you press, which is what a real circular wave does.
 * RIPPLE_MS_PER_KEY is the one knob.
 *
 * Only one pulse exists at a time: a new press restarts the ring from the new
 * key. See the note on ripple_origin for why the render thread never retires
 * the origin itself. */
#define RIPPLE_FP         8   /* sub-units per key: 1/8 key resolution  */
#define RIPPLE_MS_PER_KEY 80  /* wall clock for the front to cover a key */
#define RIPPLE_STEP_FP    ((RIPPLE_FP * EFFECTS_TICK_MS) / RIPPLE_MS_PER_KEY)
/* Ring half-width, in sub-units.
 *
 * This is NOT free to make arbitrary: it must be wider than the biggest gap
 * between neighbouring pixel distances, and on this grid those gaps reach 8
 * sub-units (the pressed key to its four neighbours is exactly 8). A band
 * narrower than the widest gap loses the ring COMPLETELY for a frame while it
 * crosses that gap, and the pulse visibly blinks out - measured, at width 4,
 * as a one-frame dropout right after the press. 6 (1.5 keys wide) clears the
 * widest gap with margin and still keeps the ring sparse: swept over every
 * origin, the most pixels ever lit at once is 14 of 15, at the instant the
 * wave covers the board, and the average is under 4 of 15. */
#define RIPPLE_RING_FP    6
/* Front radius at which the pulse is retired. Worst case is corner to
 * opposite corner: sqrt(4^2 + 2^2) = 4.47 keys = 35 sub-units, and the last
 * pixel goes dark once the front is one half-width past it, so 35 + 6 = 41.
 *
 * The bound is per BOARD, not per origin. A press near the middle has
 * finished lighting the strip long before the front reaches 41 - the centre
 * key's farthest pixel is only 17 sub-units away - so the thread keeps
 * pushing all-black frames for the remainder, up to ~20 of them. That costs
 * ~6% duty of one low-priority thread and is deliberate: the alternative is
 * tracking a per-pulse maximum distance, and this constant cannot be got
 * wrong. */
#define RIPPLE_END_FP     41
BUILD_ASSERT(RIPPLE_STEP_FP >= 1, "tick too coarse for a smooth ripple");

/* Pixel index -> keypad cell. See the grid note above. */
static inline void pixel_xy(int idx, int *col, int *row) {
    *col = idx % GRID_COLS;
    *row = idx / GRID_COLS;
}

/* Integer square root, bit-by-bit restoring method. Operands here stay below
 * 2048 (a 4 x 2 key offset at 1/8-key resolution squared) and it runs at most
 * 15 times per frame, so this is plenty and needs no library. */
static uint8_t isqrt_u16(uint16_t v) {
    uint16_t res = 0;
    uint16_t bit = 1u << 14;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= res + bit) {
            v -= res + bit;
            res = (uint16_t)((res >> 1) + bit);
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (uint8_t)res;
}

/* Called from the keypress path: arm a ring at this key. Deliberately does no
 * geometry and no SPI - the render thread does all of that on its next frame,
 * which is at most EFFECTS_TICK_MS away. Two byte stores, nothing to block. */
static void ripple_trigger(int8_t idx) {
    if (idx < 0 || idx >= LED_PIXEL_COUNT) {
        return;
    }
    ripple_origin = idx;
    ripple_r_fp   = 0;
    ripple_lit    = false;
}

static void render_ripple(void) {
    const int8_t origin = ripple_origin;

    /* Never pressed, or the ring has already run off the board. Blank once
     * and then stop pushing frames: an all-black strip does not need 660 us
     * of SPI every 10 ms. */
    if (origin < 0 || (int)ripple_r_fp >= RIPPLE_END_FP) {
        if (ripple_lit) {
            led_pixel_clear();
            led_pixel_update();
            ripple_lit = false;
        }
        return;
    }

    const int r = (int)ripple_r_fp;
    int oc, orow;
    pixel_xy(origin, &oc, &orow);

    /* Full-scale colour, scaled per pixel below - not the other way round,
     * which would apply the brightness twice. */
    const struct led_rgb c = hsv_to_rgb(rgb_control_get_hue(), 255, 255);
    const uint8_t bri   = rgb_control_get_brightness();
    const int     ring2 = RIPPLE_RING_FP * RIPPLE_RING_FP;

    led_pixel_clear();

    for (int i = 0; i < LED_PIXEL_COUNT; i++) {
        int col, row;
        pixel_xy(i, &col, &row);
        const int dx = (col - oc) * RIPPLE_FP;
        const int dy = (row - orow) * RIPPLE_FP;
        const int dist = (int)isqrt_u16((uint16_t)(dx * dx + dy * dy));

        int d = dist - r;                 /* distance from the wave front */
        if (d < 0) d = -d;
        if (d >= RIPPLE_RING_FP) continue; /* outside the band: stays black */

        /* Parabolic band, peak ON the front and symmetric either side of it. */
        const uint8_t v = (uint8_t)((bri * (ring2 - d * d)) / ring2);
        led_pixel_set((uint8_t)i, (uint8_t)((c.r * v) / 255),
                      (uint8_t)((c.g * v) / 255),
                      (uint8_t)((c.b * v) / 255));
    }

    led_pixel_update();
    ripple_lit = true;
    ripple_r_fp = (uint16_t)(r + RIPPLE_STEP_FP);
}

/* One rendered frame.
 *
 * Called ONLY from effects_thread_fn(). The self-rescheduling that used to
 * live here is now the thread's own k_sleep() loop, which is the whole point
 * of the change: the wait is no longer a system-workqueue work item. */
static void effects_render_frame(void) {
    tick_count++;
    switch (active) {
        case RGB_EFFECT_SOLID:       render_solid();      break;
        case RGB_EFFECT_BREATHING:   render_breathing();  break;
        case RGB_EFFECT_RAINBOW:     render_rainbow();    break;
        case RGB_EFFECT_SINGLE_KEY:  render_single_key(); break;
        case RGB_EFFECT_TWINKLE:     render_twinkle();    break;
        case RGB_EFFECT_RIPPLE:      render_ripple();     break;
        default: break;
    }
}

int effects_init(void) {
    return led_pixel_init();
}

void effects_tick_start(void) {
    start_tick();
}

void effects_tick_stop(void) {
    stop_tick();
}

void effects_set_active(rgb_effect_t e) {
#if defined(CONFIG_ZMK_RGB_PLAYER_BRINGUP)
    /* Bring-up build: src/bringup.c owns the strip so that every LED you
     * observe has exactly one meaning. Effect switching is intentionally inert
     * until CONFIG_ZMK_RGB_PLAYER_BRINGUP is turned back off. */
    ARG_UNUSED(e);
    return;
#endif
    active = e;
    switch (e) {
        case RGB_EFFECT_OFF:
            render_off();
            break;
        case RGB_EFFECT_SOLID:
        case RGB_EFFECT_BREATHING:
        case RGB_EFFECT_RAINBOW:
        case RGB_EFFECT_SINGLE_KEY:
        case RGB_EFFECT_TWINKLE:
        case RGB_EFFECT_RIPPLE:
            if (e == RGB_EFFECT_RIPPLE) {
                /* Clean slate: no ring until a key is pressed. Without this
                 * the last pulse would still be armed and would replay. */
                ripple_origin = -1;
                ripple_r_fp   = 0;
                ripple_lit    = false;
                led_pixel_clear();
                led_pixel_update();
            }
            start_tick();
            break;
        default: break;
    }
}

rgb_effect_t effects_get_active(void) {
    return active;
}

bool effects_is_player_active(void) {
    return player_takeover;
}

void effects_set_active_deferred(rgb_effect_t e) {
    /* Deliberately NOT effects_set_active(): that would start the render tick
     * and fight the player for the strip. Just park the value - effects_player_exit()
     * ends by calling effects_set_active(active), which applies it (and only
     * then) with the right render side effects. */
    active = e;
}

void effects_next(int direction) {
    int next = ((int)active + direction + RGB_EFFECT_COUNT) % RGB_EFFECT_COUNT;
    effects_set_active((rgb_effect_t)next);
}

void effects_on_key_down(int8_t key_index) {
#if defined(CONFIG_ZMK_RGB_PLAYER_BRINGUP)
    bringup_key_event(key_index, true);
    return;
#endif
    rgb_control_set_last_key(key_index);
    ripple_trigger(key_index);
    if (active == RGB_EFFECT_SINGLE_KEY) {
        active_key = key_index;
        render_single_key();
    }
    /* No render for the ripple here: the render thread draws the new ring on
     * its next frame, at most EFFECTS_TICK_MS away. */
}

void effects_on_key_up(int8_t key_index) {
#if defined(CONFIG_ZMK_RGB_PLAYER_BRINGUP)
    bringup_key_event(key_index, false);
    return;
#endif
    if (active == RGB_EFFECT_SINGLE_KEY && active_key == key_index) {
        active_key = -1;
        led_pixel_clear();
        led_pixel_update();
    }
}

void effects_set_active_key(int8_t key_index) {
    effects_on_key_down(key_index);
}

/* ------------------------------------------------------------------ */
/* per-key light feedback, driven straight off the kscan event         */
/* ------------------------------------------------------------------ */
/* The keymap used to route every key through the hand-rolled &kp_we, which
 * happened to call effects_on_key_down() on its way past. KPTEST moved the
 * typing path onto ZMK's stock &kp - right for typing, but it also removed
 * those calls and so silently broke the two effects that need to know WHICH key
 * was pressed:
 *
 *   RGB_EFFECT_SINGLE_KEY   lights the key you are holding
 *   RGB_EFFECT_RIPPLE       radiates the wave from the last key pressed
 *
 * Rather than re-attach them through the keymap - where one bad binding can
 * take typing down with it - subscribe to ZMK's own position event. It fires
 * for every key, BEFORE the keymap is consulted, so per-key light feedback now
 * survives even a broken keymap. That is the same reasoning that made the
 * bring-up marker kscan-driven.
 *
 * ev->position is the KEYMAP position (0..14): the matrix transform has already
 * been applied, which is exactly the LED index this strip uses (one pixel per
 * key, wired in keymap order).
 *
 * WHY THIS RECORDS STATE BUT DELIBERATELY DOES NOT RENDER - unlike every other
 * caller of effects_on_key_*():
 *
 * This callback runs INSIDE THE INPUT PATH, i.e. the very call chain the
 * keystroke itself is travelling down, and led_pixel_update() is a SYNCHRONOUS
 * SPI burst (~1.2 ms for one 360-byte frame) taken under led_mutex. Rendering
 * from here would add that stall - plus however long an in-flight tick render
 * still holds the mutex - to EVERY key press. That is a real price to pay in
 * the one revision whose entire purpose is input latency, and it buys a light
 * that nobody can perceive as late. So this handler only records what changed;
 * effects_render_frame() renders it within one 20 ms frame.
 *
 * effects_on_key_down()/up() themselves KEEP their immediate render - the song
 * player (player.c) and the BLE note service (ble_service.c) both call them
 * and both want the pixel lit at the instant they say so.
 *
 * On concurrency: the two fields written here (rgb_control's last_key and
 * active_key) are plain, aligned int8_t stores, which the M4 cannot tear, and
 * effects_render_frame() re-reads them from scratch every frame rather than
 * accumulating. A store landing mid-tick therefore costs at most one frame of
 * lag, never a corrupt value - so no lock is needed to protect them. Whether
 * this runs on the system workqueue (kscan's scan is a k_work_delayable in
 * ZMK's gpio-matrix driver) or on a kscan thread, the argument is the same. */
static int effects_position_cb(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

#if defined(CONFIG_ZMK_RGB_PLAYER_BRINGUP)
    /* Bring-up build: src/bringup.c owns the strip and does its own counting,
     * so hand the press straight over - same as effects_on_key_down() would. */
    bringup_key_event((int8_t)ev->position, ev->state);
#else
    rgb_control_set_last_key((int8_t)ev->position);
    if (ev->state) {
        /* Press only - a ring on release would double every keystroke. */
        ripple_trigger((int8_t)ev->position);
    }

    /* Mirror effects_on_key_up()'s guard: only the key that is actually
     * lighting the strip may clear it, so releasing an earlier key while a
     * later one is held does not blank the held key's pixel. */
    if (active == RGB_EFFECT_SINGLE_KEY) {
        if (ev->state) {
            active_key = (int8_t)ev->position;
        } else if (active_key == (int8_t)ev->position) {
            active_key = -1;
        }
    }
#endif
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(effects_position, effects_position_cb);
ZMK_SUBSCRIPTION(effects_position, zmk_position_state_changed);

void effects_player_enter(void) {
    player_takeover = true;
    stop_tick();
    led_pixel_clear();
    led_pixel_update();
}

void effects_player_exit(void) {
    player_takeover = false;
    led_pixel_clear();
    led_pixel_update();
    effects_set_active(active);
}