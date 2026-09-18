/*
 * mode_c.c - Assisted Play-Along ("引导弹奏") mode
 *
 * Guided, user-paced playback. The keyboard claims the 15-pixel WS2812B strip
 * (effects_player_enter) and renders:
 *   - current step key  -> BLUE
 *   - next   step key   -> RED
 * STRICTLY USER-PACED: the cursor never moves on its own - only the correct
 * physical press advances it.
 * On a correct press: GREEN flash -> wait `delta` ms -> next step arms.
 * On a wrong press:   ALL RED flash -> MISS event, stay on this step.
 * On no press:        AMBER pulse on the expected key -> stay on this step.
 *                     (There is no timeout-skip any more.)
 *
 * Why that changed: the previous build auto-advanced every 300 ms. `duration` is
 * a uint8 that mc_arm_step() floored to 300, so an untouched keyboard played the
 * whole score by itself - the run was never actually step-by-step.
 *
 * Physical key presses are captured from the ZMK position_state_changed event
 * so ALL 15 keys are detected, including the four corners that are bound to
 * &none in the keymap (they still generate a position event).
 *
 * Timing is driven entirely by the user; MODE_C_TICK only refreshes an
 * informational reference time.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/led_strip.h>
#include <zmk/events/position_state_changed.h>
#include <zmk_rgbeffect/mode_c.h>
#include <zmk_rgbeffect/effects.h>
#include <zmk_rgbeffect/led_pixel.h>
#include <zmk_rgbeffect/ble_service.h>

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

typedef enum { MC_IDLE, MC_RUNNING, MC_ADVANCING } mc_state_t;

typedef struct __attribute__((packed)) {
    uint8_t  key;       /* 1..15 */
    uint8_t  duration;  /* ms, reminder interval upper bound (no timeout-skip) */
    uint16_t delta;     /* ms, gap after a HIT before the next step arms */
} mc_step_t;

static mc_step_t steps[MODE_C_MAX_STEPS];
static uint16_t  step_count = 0;   /* number of PUSHed steps */
static uint16_t  cur_step   = 0;
static mc_state_t state     = MC_IDLE;
static uint32_t  app_ref_ms = 0;

static struct k_work_delayable timeout_work;
static struct k_work_delayable advance_work;
static struct k_work_delayable restore_work;

static void mc_advance(void);
static void mc_arm_reminder(void);

/* Cue colors (GRB order not needed; led_rgb is r,g,b). */
static const struct led_rgb CUE_BLUE  = { .r = 0,   .g = 40,  .b = 255 };
static const struct led_rgb CUE_RED   = { .r = 255, .g = 0,   .b = 0   };
static const struct led_rgb CUE_GREEN = { .r = 0,   .g = 255, .b = 0   };
/* Reminder pulse. The expected key is already lit BLUE, so the "still waiting on
 * you" nudge has to be a different colour to register as a nudge at all. */
static const struct led_rgb CUE_AMBER = { .r = 255, .g = 110, .b = 0   };

/* How long a step waits before pulsing that reminder, in ms.
 *
 * This used to be `steps[].duration`, the per-step timeout - but that field is a
 * uint8 (so <= 255) and mc_arm_step() floored it at 300, meaning every step
 * elapsed after 300 ms and the guide advanced by itself. The wire field is kept
 * for compatibility and used here only as an upper bound; the floor is what
 * actually governs, and nothing in this file advances on it any more. */
#define MODE_C_REMIND_MIN_MS 1200u
#define MODE_C_REMIND_MAX_MS 8000u

/* ---- rendering ---- */

static void render_blue_red(void) {
    led_pixel_clear();
    if (cur_step >= step_count) { led_pixel_update(); return; }
    int cur = (int)steps[cur_step].key - 1;
    if (cur >= 0 && cur < LED_PIXEL_COUNT) {
        led_pixel_set((uint8_t)cur, CUE_BLUE.r, CUE_BLUE.g, CUE_BLUE.b);
    }
    if (cur_step + 1 < step_count) {
        int nxt = (int)steps[cur_step + 1].key - 1;
        if (nxt >= 0 && nxt < LED_PIXEL_COUNT) {
            led_pixel_set((uint8_t)nxt, CUE_RED.r, CUE_RED.g, CUE_RED.b);
        }
    }
    led_pixel_update();
}

static void flash_one(int idx, const struct led_rgb c, int ms) {
    led_pixel_clear();
    if (idx >= 0 && idx < LED_PIXEL_COUNT) {
        led_pixel_set((uint8_t)idx, c.r, c.g, c.b);
    }
    led_pixel_update();
    k_work_schedule(&restore_work, K_MSEC(ms));
}

static void flash_all(const struct led_rgb c, int ms) {
    led_pixel_clear();
    for (int i = 0; i < LED_PIXEL_COUNT; i++) {
        led_pixel_set((uint8_t)i, c.r, c.g, c.b);
    }
    led_pixel_update();
    k_work_schedule(&restore_work, K_MSEC(ms));
}

/* ---- work handlers ---- */

static void timeout_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (state != MC_RUNNING) return;
    /* Reminder only. This handler used to flash the key RED, report TIMEOUT and
     * skip to the next step - which is exactly the "the score plays itself"
     * behaviour we are removing. A step now waits for the user indefinitely. */
    int cur = (int)steps[cur_step].key - 1;
    flash_one(cur, CUE_AMBER, 180);
    mc_arm_reminder();
}

static void advance_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mc_advance();
}

static void restore_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    /* Only restore the blue/red display if we are still parked on a step
     * (not mid-advance, where the advance work owns the next render). */
    if (state == MC_RUNNING) {
        render_blue_red();
    }
}

/* ---- state machine ---- */

/* Arm the "you are still on this step" pulse. Advances nothing: the only way
 * out of a step is mode_c_on_position() seeing the correct key. */
static void mc_arm_reminder(void) {
    uint32_t to = steps[cur_step].duration;
    if (to < MODE_C_REMIND_MIN_MS) to = MODE_C_REMIND_MIN_MS;
    if (to > MODE_C_REMIND_MAX_MS) to = MODE_C_REMIND_MAX_MS;
    k_work_schedule(&timeout_work, K_MSEC(to));
}

static void mc_arm_step(void) {
    state = MC_RUNNING;
    render_blue_red();
    mc_arm_reminder();
}

static void mc_advance(void) {
    cur_step++;
    if (cur_step >= step_count) {
        state = MC_IDLE;
        led_pixel_clear();
        led_pixel_update();
        effects_player_exit();   /* release the strip back to user effects */
        mode_c_notify_event(MODE_C_EVT_DONE, 0, step_count);
        LOG_INF("mode_c: finished %u steps", step_count);
        return;
    }
    mc_arm_step();
}

/* ---- public API ---- */

void mode_c_on_position(uint32_t position, bool pressed) {
    if (!pressed) return;                 /* act only on press */
    if (state != MC_RUNNING) return;
    if (position >= LED_PIXEL_COUNT) return;

    uint8_t expected    = steps[cur_step].key;     /* 1..15 */
    uint8_t pressed_key = (uint8_t)(position + 1);  /* position 0..14 -> 1..15 */

    if (pressed_key == expected) {
        /* HIT */
        flash_one((int)position, CUE_GREEN, 140);
        mode_c_notify_event(MODE_C_EVT_HIT, pressed_key, cur_step);
        k_work_cancel_delayable(&timeout_work);
        state = MC_ADVANCING;
        uint32_t gap = steps[cur_step].delta;
        if (gap < 80) gap = 140;     /* minimum so the green flash reads */
        if (gap > MODE_C_DELTA_MAX_MS) gap = MODE_C_DELTA_MAX_MS;
                                 /* was hard-coded to 1000; raised so long
                                    rests keep their tempo (negotiated via
                                    BE04 status byte 8, bit0) */
        k_work_schedule(&advance_work, K_MSEC(gap));
    } else {
        /* MISS: flash all red, report, keep waiting on this step. */
        flash_all(CUE_RED, 220);
        mode_c_notify_event(MODE_C_EVT_MISS, pressed_key, cur_step);
        k_work_cancel_delayable(&timeout_work);
        mc_arm_reminder();
    }
}

void mode_c_start(uint16_t note_count) {
    if (state == MC_RUNNING || state == MC_ADVANCING) {
        mode_c_stop();
    }
    if (step_count == 0) {
        LOG_WRN("mode_c_start: no steps pushed yet");
        return;
    }
    cur_step = 0;
    effects_player_enter();   /* claim the LED strip, stop effect tick */
    LOG_INF("mode_c_start: %u steps (requested %u)", step_count, note_count);
    mc_arm_step();
}

void mode_c_push(uint16_t delta_ms, uint8_t key, uint8_t duration_ms) {
    if (step_count >= MODE_C_MAX_STEPS) {
        LOG_WRN("mode_c_push: buffer full");
        return;
    }
    if (key < 1 || key > 15) {
        LOG_WRN("mode_c_push: bad key %u (need 1..15)", key);
        return;
    }
    steps[step_count].key      = key;
    steps[step_count].duration = duration_ms;
    steps[step_count].delta    = delta_ms;
    step_count++;
}

void mode_c_tick(uint32_t app_ms) {
    app_ref_ms = app_ms;
    /* Informational only; Mode C timing lives on the keyboard clock.
     * The app reference is logged so it can be correlated in a bug report. */
    LOG_DBG("mode_c_tick: app_ref_ms=%u", app_ref_ms);
}

void mode_c_stop(void) {
    k_work_cancel_delayable(&timeout_work);
    k_work_cancel_delayable(&advance_work);
    k_work_cancel_delayable(&restore_work);
    state = MC_IDLE;
    led_pixel_clear();
    led_pixel_update();
    effects_player_exit();
    LOG_INF("mode_c_stop");
}

bool mode_c_is_active(void) {
    return state == MC_RUNNING || state == MC_ADVANCING;
}

uint16_t mode_c_current_step(void) {
    return cur_step;
}

/* ---- init + ZMK position listener ---- */

static int mode_c_position_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev =
        as_zmk_position_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    mode_c_on_position(ev->position, ev->state);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(mode_c_position, mode_c_position_listener)
ZMK_SUBSCRIPTION(mode_c_position, zmk_position_state_changed)

int mode_c_init(void) {
    k_work_init_delayable(&timeout_work, timeout_work_fn);
    k_work_init_delayable(&advance_work, advance_work_fn);
    k_work_init_delayable(&restore_work, restore_work_fn);
    state = MC_IDLE;
    step_count = 0;
    cur_step = 0;
    return 0;
}
