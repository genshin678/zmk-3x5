/*
 * rgb_control.c - global brightness / hue / effect / last-key state
 *
 * Holds the shared hue/brightness/last-key state used by all of the
 * module's self-rendered effects (solid/breathing/rainbow/single/...).
 */
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <zmk_rgbeffect/rgb_control.h>
#include <zmk_rgbeffect/effects.h>

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

/* Hard ceiling on the global brightness.
 *
 * 15 WS2812B draw up to 60 mA each at full white, i.e. ~900 mA for the strip
 * alone; at the old default of 128/255 some effects still pulled ~450 mA. That
 * is a lot to ask of a board whose LEDs were only just reworked onto their own
 * 5 V feed, and it is a live suspect for any input misbehaviour that appears
 * only while the strip is lit (a sagging rail browns out the MCU; the boot
 * signature would replay).
 *
 * 96/255 (~38%) is still clearly visible for every effect and keeps the strip
 * worst case near ~340 mA. The clamp lives in rgb_control_set_brightness() so
 * EVERY path is bounded - the boot default, the Y+P brightness loop, and the
 * BLE service - rather than relying on each caller to behave. */
#define RGB_MAX_BRIGHTNESS 96

static uint8_t   brightness   = RGB_MAX_BRIGHTNESS;
static uint16_t  hue         = 200;
static int8_t    last_key    = -1;

/* Persisted light configuration (effect / brightness / hue).
 *
 * The handler below is registered statically, so ZMK's settings subsystem
 * calls the set callback during its POST_KERNEL settings_load() - which runs
 * BEFORE this module's APPLICATION-level init. The callback therefore only
 * parks the bytes here; rgb_control_init() (below) copies them into the live
 * state, and module_init.c picks the effect up once effects_init() has bound
 * the strip. Nothing here may touch effects_set_active() directly. */
static struct {
    uint8_t  effect;
    uint8_t  brightness;
    uint16_t hue;
} saved_cfg;
static bool saved_valid = false;

/* settings set callback: reads the stored "cfg" blob into saved_cfg. */
static int light_cfg_set(const char *name, size_t len,
                         settings_read_cb read_cb, void *cb_arg) {
    if (strcmp(name, "cfg") != 0 || len != sizeof(saved_cfg)) {
        return 0;
    }
    uint8_t buf[sizeof(saved_cfg)];
    ssize_t got = read_cb(cb_arg, buf, len);
    if (got != (ssize_t)len) {
        return 0;
    }
    memcpy(&saved_cfg, buf, len);
    saved_valid = true;
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(rgb_light, "rgbl", NULL, light_cfg_set, NULL, NULL);

/* Brightness loop state (Y+P simultaneous hold). */
static bri_loop_t  bri_loop   = BRI_IDLE;
static int8_t      bri_loop_dir = 0;
static struct k_work_delayable bri_loop_work;

/* Hue loop state (H+; simultaneous hold). */
static int8_t      hue_loop_dir = 0;   /* +1 or -1 */
static struct k_work_delayable hue_loop_work;

/* One step of the Y+P brightness loop.
 *
 * This WRAPS rather than parking at the ceiling: going past
 * RGB_MAX_BRIGHTNESS lands on 0 and climbing resumes, and going below 0 lands
 * back on the ceiling. That is the point - RGB_MAX_BRIGHTNESS is a power /
 * current ceiling, not a place for the loop to stop. It also fixes the dead
 * combo: the boot default used to sit AT that ceiling while the only direction
 * was +1, so every step ran straight into the clamp and nothing ever changed.
 *
 * This is deliberately NOT rgb_control_change_brightness(): that one clamps,
 * which is the right semantic for a plain "nudge the value" API. The wrap
 * belongs to the *loop* alone, and BOTH loop entry points go through here so
 * they cannot drift apart. */
static void bri_step(int delta) {
    int v = (int)brightness + delta;
    if (v > RGB_MAX_BRIGHTNESS) v = 0;
    if (v < 0)                  v = RGB_MAX_BRIGHTNESS;
    brightness = (uint8_t)v;
}

static void bri_loop_tick(struct k_work *work) {
    if (bri_loop == BRI_IDLE) return;
    bri_step((bri_loop == BRI_UP) ? +1 : -1);
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

    /* settings_load() has already run at this point (POST_KERNEL), so a stored
     * configuration is available now - adopt it. Only brightness/hue are applied
     * here; the effect needs effects_init() to have bound the strip first, so
     * module_init.c reads it via rgb_control_get_saved_effect(). */
    if (saved_valid) {
        rgb_control_set_brightness(saved_cfg.brightness);
        rgb_control_set_hue(saved_cfg.hue);
        LOG_INF("restored light cfg: effect=%u bright=%u hue=%u",
                (unsigned)saved_cfg.effect, (unsigned)saved_cfg.brightness,
                (unsigned)saved_cfg.hue);
    }
    return 0;
}

void rgb_control_set_brightness(uint8_t v) {
    /* Single clamp point for the whole module - see RGB_MAX_BRIGHTNESS. */
    if (v > RGB_MAX_BRIGHTNESS) {
        v = RGB_MAX_BRIGHTNESS;
    }
    brightness = v;
}
void rgb_control_change_brightness(int delta) {
    int v = (int)brightness + delta;
    if (v < 0)                   v = 0;
    if (v > RGB_MAX_BRIGHTNESS)  v = RGB_MAX_BRIGHTNESS;
    rgb_control_set_brightness((uint8_t)v);
}
uint8_t rgb_control_get_brightness(void) { return brightness; }

void rgb_control_set_hue(uint16_t h) {
    hue = h % 360;
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
    bri_step(bri_loop_dir);
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

/* Persist the current light configuration to NVS.
 *
 * Driven by the BLE LIGHT_CTRL_SAVE command rather than by every setter: a
 * brightness slider would otherwise write flash on every drag step. */
void rgb_control_save(void) {
    saved_cfg.effect     = (uint8_t)effects_get_active();
    saved_cfg.brightness = brightness;
    saved_cfg.hue        = hue;
    saved_valid = true;
    settings_save_one("rgbl/cfg", &saved_cfg, sizeof(saved_cfg));
}

/* The effect restored from NVS, or false when there is none (first boot), so
 * module_init.c can fall back to CONFIG_ZMK_RGB_PLAYER_DEFAULT_EFFECT. */
bool rgb_control_get_saved_effect(rgb_effect_t *out) {
    if (!saved_valid || out == NULL) {
        return false;
    }
    if (saved_cfg.effect >= RGB_EFFECT_COUNT) {
        return false;   /* corrupt / from a build with fewer effects */
    }
    *out = (rgb_effect_t)saved_cfg.effect;
    return true;
}