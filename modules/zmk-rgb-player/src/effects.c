/*
 * effects.c - 6 effect routines on the 15-pixel WS2812B strip
 *
 * Effect 0 = OFF (all pixels dark, no tick).
 * Effect 1/2/3 = ZMK built-in solid / breathing / rainbow (ZMK drives them).
 * Effect 4/5/6 = per-pixel effects driven by our k_work_delayable tick.
 *
 * For effect 6 (ripple), the wave origin is rgb_control_get_last_key().
 *
 * All 7 effects are rendered by this module directly via the WS2812B
 * driver; none rely on ZMK's built-in rgb_underglow subsystem.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
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
 * spi-zero-frame comment in the shield overlay) - which at 3.2 MHz takes
 * ~900 us, plus the 300 us latch gap the newer WS2812B dies need, so ~1.2 ms
 * per frame. And it is SYNCHRONOUS: led_pixel_update() ->
 * led_strip_update_rgb() -> spi_write() does not return until the DMA has
 * drained.
 *
 * At a 10 ms tick that is 12% of the SYSTEM WORKQUEUE pushing pixels - and the
 * system workqueue is the same queue ZMK scans the key matrix on, so every
 * render delays input handling slightly. At 20 ms it is 6%, and the strip
 * still renders at 50 fps, which is smooth for all seven effects.
 *
 * CORRECTION, for the record: an earlier revision of bringup.c claimed one
 * frame was 2880 bytes / 7.2 ms. That was wrong by a factor of 8 - it counted
 * the 8 SPI bits inside a frame byte as though each were its own byte. The
 * real figure is 360 bytes / ~1.2 ms, so the strip was never the load it was
 * believed to be and disabling it saved far less than assumed. (It is
 * re-enabled in this revision, the hardware now having a working 5 V feed.)
 *
 * The animation constants below - the breathing divider, the rainbow hue step
 * and RIPPLE_SPEED - are scaled so that each effect keeps the wall-clock speed
 * it had at 10 ms. */
#define EFFECTS_TICK_MS 20

static struct k_work_delayable effects_work;
static rgb_effect_t active = RGB_EFFECT_OFF;
static int8_t active_key = -1;         /* for SINGLE_KEY effect */
static bool player_takeover = false;
static uint32_t tick_count = 0;
static bool tick_running;

static void effects_tick(struct k_work *work);

static void start_tick(void) {
    if (tick_running) return;
    k_work_init_delayable(&effects_work, effects_tick);
    k_work_schedule(&effects_work, K_MSEC(EFFECTS_TICK_MS));
    tick_running = true;
}

static void stop_tick(void) {
    if (!tick_running) return;
    k_work_cancel_delayable(&effects_work);
    tick_running = false;
}

static struct led_rgb hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v) {
    struct led_rgb c = {0, 0, 0};
    if (s == 0) { c.r = c.g = c.b = v; return c; }
    uint8_t region = h / 60;
    uint8_t rem    = (h - region * 60) * 6;
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
    /* /8, not /16: EFFECTS_TICK_MS was doubled, so halving this divider keeps
     * the breathing period at the same ~16 s wall-clock instead of stretching
     * it to 32 s. */
    uint32_t t = (tick_count / 8) % 100;
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

static void render_rainbow(void) {
    for (int i = 0; i < LED_PIXEL_COUNT; i++) {
        /* *6, not *3: the tick was doubled, so the hue step doubles too and a
         * full 360-degree sweep still takes the same ~1.2 s. */
        uint16_t hue = (uint16_t)((tick_count * 6 + i * 24) % 360);
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

static void render_twinkle(void) {
    static uint8_t decay[LED_PIXEL_COUNT];
    static uint8_t r[LED_PIXEL_COUNT], g[LED_PIXEL_COUNT], b[LED_PIXEL_COUNT];
    for (int i = 0; i < LED_PIXEL_COUNT; i++) {
        if (decay[i] > 0) {
            decay[i] = (decay[i] > 30) ? (decay[i] - 30) : 0;
        } else {
            uint32_t roll = (tick_count * 1103515245u + i * 12345u + 7) & 0xFF;
            if (roll < 20) {
                uint16_t hue = (rgb_control_get_hue() + i * 24 + roll) % 360;
                struct led_rgb c = hsv_to_rgb(hue, 255,
                                              rgb_control_get_brightness());
                r[i] = c.r; g[i] = c.g; b[i] = c.b;
                decay[i] = 255;
            } else {
                r[i] = g[i] = b[i] = 0;
            }
        }
        led_pixel_set((uint8_t)i, r[i], g[i], b[i]);
    }
    led_pixel_update();
}

/*
 * Ripple: a wave of 3 lit pixels radiates outward from the last-pressed
 * key (last_key), then fades.  Waves from multiple keys superpose.
 *
 * Wave parameters:
 *   speed   = 1 key per 12 ticks  (~120ms at 10ms tick)
 *   width   = 5 keys (±2 around the head)
 *   max_dist = 14 (half the strip)
 */
/* 6, not 12: one key every 6 ticks instead of 12, so at a 20 ms tick the wave
 * still advances one key per ~120 ms as it did at 10 ms. */
#define RIPPLE_SPEED   6
#define RIPPLE_WIDTH   5
#define RIPPLE_MAX_D   14

static void render_ripple(void) {
    led_pixel_clear();

    int8_t origin = rgb_control_get_last_key();
    if (origin < 0 || origin >= LED_PIXEL_COUNT) {
        led_pixel_update();
        return;
    }

    uint16_t hue = rgb_control_get_hue();
    uint8_t  bri = rgb_control_get_brightness();
    uint8_t  dist_base = (tick_count / RIPPLE_SPEED) % (RIPPLE_MAX_D * 2 + 1);

    for (int offset = -RIPPLE_WIDTH / 2; offset <= RIPPLE_WIDTH / 2; offset++) {
        /* Positive wave (rightward) */
        int d_pos = dist_base + offset;
        if (d_pos >= 0 && d_pos <= RIPPLE_MAX_D) {
            int idx = origin + d_pos;
            if (idx < LED_PIXEL_COUNT) {
                uint8_t falloff = (uint8_t)(255 * (RIPPLE_WIDTH / 2 - offset) / (RIPPLE_WIDTH / 2 + 1));
                uint8_t v = (uint8_t)((bri * falloff) / 255);
                struct led_rgb c = hsv_to_rgb((hue + d_pos * 15) % 360, 255, v);
                led_pixel_set((uint8_t)idx, c.r, c.g, c.b);
            }
        }
        /* Negative wave (leftward) */
        int d_neg = dist_base + offset;
        if (d_neg >= 0 && d_neg <= RIPPLE_MAX_D) {
            int idx = origin - d_neg;
            if (idx >= 0) {
                uint8_t falloff = (uint8_t)(255 * (RIPPLE_WIDTH / 2 - offset) / (RIPPLE_WIDTH / 2 + 1));
                uint8_t v = (uint8_t)((bri * falloff) / 255);
                struct led_rgb c = hsv_to_rgb((hue + d_neg * 15) % 360, 255, v);
                led_pixel_set((uint8_t)idx, c.r, c.g, c.b);
            }
        }
    }
    led_pixel_update();
}

static void effects_tick(struct k_work *work) {
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
    if (tick_running) {
        k_work_schedule(&effects_work, K_MSEC(EFFECTS_TICK_MS));
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
            start_tick();
            break;
        default: break;
    }
}

rgb_effect_t effects_get_active(void) {
    return active;
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
    if (active == RGB_EFFECT_SINGLE_KEY) {
        active_key = key_index;
        render_single_key();
    }
    /* Ripple effect uses last_key to set wave origin - no render here
     * because the tick() will pick it up on the next 10ms tick. */
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