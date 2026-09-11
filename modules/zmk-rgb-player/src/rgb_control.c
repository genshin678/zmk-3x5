/*
 * rgb_control.c - global brightness / hue / effect / last-key state
 *
 * Mirrors state into ZMK's &rgb_ug so built-in effects (solid/breathing/
 * rainbow) and our per-pixel effects share the same hue/brightness.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk/rgb_underglow.h>
#include <zmk_rgbeffect/rgb_control.h>
#include <zmk_rgbeffect/effects.h>

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_LOG_LEVEL);

static uint8_t   brightness   = 128;
static uint16_t  hue         = 200;
static int8_t    last_key    = -1;

/* Brightness loop state (Y+P simultaneous hold). */
static bri_loop_t  bri_loop   = BRI_IDLE;
static int8_t      bri_loop_dir = 0;
static struct k_work_delayable bri_loop_work;

/* Hue loop state (H+; simultaneous hold). */
static int8_t      hue_loop_dir = 0;   /* +1 or -1 */
static struct k_work_delayable hue_loop_work;

static void bri_loop_tick(struct k_work *work) {
    if (bri_loop == BRI_IDLE) return;
    int delta = (bri_loop == BRI_UP) ? +1 : -1;
    rgb_control_change_brightness(delta);
    k_work_schedule(&bri_loop_work, K_MSEC(100));
}

static void hue_loop_tick(struct k_work *work) {
    if (hue_loop_dir == 0) return;
    rgb_control_change_hue(hue_loop_dir * 8);
    k_work_schedule(&hue_loop_work, K_MSEC(100));
}

int rgb_control_init(void) {
    k_work_init_delayable(&bri_loop_work, bri_loop_tick);
    k_work_init_delayable(&hue_loop_work, hue_loop_tick);
    zmk_rgb_underglow_off();
    return 0;
}

void rgb_control_set_brightness(uint8_t v) {
    brightness = v;
    zmk_rgb_underglow_set_brightness(v);
}
void rgb_control_change_brightness(int delta) {
    int v = (int)brightness + delta;
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    rgb_control_set_brightness((uint8_t)v);
}
uint8_t rgb_control_get_brightness(void) { return brightness; }

void rgb_control_set_hue(uint16_t h) {
    hue = h % 360;
    zmk_rgb_underglow_set_hsv(hue, 100, brightness);
}
void rgb_control_change_hue(int delta) {
    int v = (int)hue + delta;
    while (v < 0)    v += 360;
    while (v >= 360) v -= 360;
    rgb_control_set_hue((uint16_t)v);
}
uint16_t rgb_control_get_hue(void) { return hue; }

rgb_effect_t rgb_control_get_effect(void) {
    return effects_get_active();
}
void rgb_control_set_effect(rgb_effect_t e) {
    effects_set_active(e);
}
void rgb_control_next_effect(void) {
    effects_next(+1);
}

/* Brightness loop (Y+P simultaneous hold). */
void rgb_control_bri_loop_start(bri_loop_t dir) {
    if (bri_loop != BRI_IDLE && bri_loop != dir) {
        /* Direction reversed mid-loop: update and keep going. */
    }
    bri_loop = dir;
    bri_loop_dir = (dir == BRI_UP) ? +1 : -1;
    k_work_cancel_delayable(&bri_loop_work);
    rgb_control_change_brightness(bri_loop_dir);
    k_work_schedule(&bri_loop_work, K_MSEC(100));
}
void rgb_control_bri_loop_stop(void) {
    bri_loop = BRI_IDLE;
    bri_loop_dir = 0;
    k_work_cancel_delayable(&bri_loop_work);
}
bri_loop_t rgb_control_get_bri_loop(void) { return bri_loop; }

/* Hue loop (H+; simultaneous hold). */
void rgb_control_hue_loop_start(int8_t dir) {
    if (hue_loop_dir != 0 && hue_loop_dir != dir) {
        /* Reversed: update and keep going. */
    }
    hue_loop_dir = dir;
    k_work_cancel_delayable(&hue_loop_work);
    rgb_control_change_hue(dir * 8);
    k_work_schedule(&hue_loop_work, K_MSEC(100));
}
void rgb_control_hue_loop_stop(void) {
    hue_loop_dir = 0;
    k_work_cancel_delayable(&hue_loop_work);
}
int8_t rgb_control_get_hue_loop_dir(void) { return hue_loop_dir; }

/* Last key (for ripple wave origin). */
void rgb_control_set_last_key(int8_t k) { last_key = k; }
int8_t  rgb_control_get_last_key(void)  { return last_key; }