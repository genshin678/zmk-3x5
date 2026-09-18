/*
 * mode_c.c - Assisted Play-Along ("引导弹奏")
 *
 * Guided playback over the 15-pixel WS2812B strip.
 *
 * ONE STEP = ONE CELL = ONE CHORD. `mask` is a set of keys (bit k-1 for key k in
 * 1..15) and a step is satisfied only once every key in it has been pressed. That
 * mirrors how the source score groups notes: the App used to expand a chord into
 * several consecutive single-key steps, which turned a chord into a fast arpeggio
 * and made the lights disagree with the printed sheet.
 *
 * PACING - switchable at run time with 0x36 MODE_C_PACE, mid-session included:
 *
 *   MANUAL  Strictly user-paced. Nothing here advances on a clock: a step clears
 *           only when all of its keys have been pressed, in any order (a rolled
 *           chord counts - completeness is required, simultaneity is not). A wrong
 *           key flashes red and reports MISS but does not move the cursor.
 *   AUTO    The keyboard plays the score itself: each step is shown for its own
 *           `delta` and then advances. Physical presses are not judged.
 *
 * Fingers that stay down carry over: when a step is armed, keys already held that
 * belong to it are counted immediately, so a chord sharing a finger with the one
 * before it cannot deadlock waiting for a press that will never come.
 *
 * RENDERING - the current step's keys are BLUE and the next step's keys (minus any
 * key already blue: a note shared by both must keep saying "press this now") are
 * RED. There is no amber "hurry up" pulse; a step simply waits.
 *
 * Why the previous behaviour changed: `duration` is a uint8 and mc_arm_step()
 * floored it at 300 ms, so an untouched keyboard walked the whole score by itself -
 * the run was never really step-by-step. Nothing times out any more.
 *
 * Physical key presses arrive from ZMK's position_state_changed, so all 15 keys are
 * detected, including the four corner keys bound to &none in the keymap: a press
 * there still generates a position event and still counts as a wrong key.
 *
 * Timing is driven by the score; MODE_C_TICK only refreshes an informational
 * reference time for log correlation.
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
    uint16_t delta;    /* ms: post-HIT gap (manual) / the step's own length (auto) */
    uint16_t mask;     /* bit(k-1) for key k in 1..15; never 0 */
    uint8_t  duration; /* ms; informational - nothing in this file times out */
} mc_step_t;

static mc_step_t steps[MODE_C_MAX_STEPS];
static uint16_t  step_count = 0;   /* number of PUSHed steps */
static uint16_t  cur_step   = 0;
static mc_state_t state     = MC_IDLE;
static uint8_t   pace       = MODE_C_PACE_MANUAL;
static uint16_t  pressed_mask = 0; /* keys physically down right now */
static uint16_t  hit_mask     = 0; /* keys of the current step already satisfied */
static uint32_t  app_ref_ms   = 0;

static struct k_work_delayable step_work;    /* AUTO: the step's own clock */
static struct k_work_delayable advance_work; /* post-HIT gap -> next step */
static struct k_work_delayable restore_work; /* undo a flash */

static void mc_advance(void);

/* Cue colors (led_rgb is r,g,b; the ST-1209RGB G,R,B wire order is handled by the
 * driver, which is where that measurement belongs). */
static const struct led_rgb CUE_BLUE  = { .r = 0,   .g = 40,  .b = 255 };
static const struct led_rgb CUE_RED   = { .r = 255, .g = 0,   .b = 0   };
static const struct led_rgb CUE_GREEN = { .r = 0,   .g = 255, .b = 0   };

/* ---- release-verification build tag ----
 *
 * CONFIG_LOG=n compiles every LOG_* macro away, arguments included, so no log string
 * survives into the image and there is nothing text-shaped to look for in a released
 * .uf2. This blob is the substitute: it is plain .rodata, and it can be scanned out of
 * the flashed image to answer "is this board really running the chord-mask build?".
 *
 * It is reached through a volatile POINTER, not a constant index. A plain constant subscript
 * folded into an immediate by the compiler, which leaves the array unreferenced and
 * lets --gc-sections drop it - the build still succeeds, the module is still
 * warning-free, and the tag is silently missing from the image. That is what the first
 * attempt at this did. A volatile pointer forces a real load, hence a relocation, hence
 * a section the linker has to keep; __attribute__((used)) is belt and braces. */
static const uint8_t mc_build_tag[] __attribute__((used)) =
    "MCV2/CHORD-MASK+MANUAL-AUTO-PACE";
static const uint8_t *volatile mc_build_tag_probe = mc_build_tag;
static volatile uint8_t mc_build_tag_sink;

/* ---- helpers ---- */

static uint8_t lowest_key(uint16_t mask) {
    for (uint8_t k = 1; k <= 15; k++) {
        if (mask & (uint16_t)(1u << (k - 1))) {
            return k;
        }
    }
    return 0;
}

/* Deliberately not __builtin_popcount: on arm-none-eabi that lowers to a libgcc
 * call (__popcountsi2) for no benefit here - this runs once per step. */
static uint8_t chord_size(uint16_t mask) {
    uint8_t n = 0;
    for (uint8_t k = 0; k < 15; k++) {
        if (mask & (uint16_t)(1u << k)) {
            n++;
        }
    }
    return n;
}

static uint32_t clamp_delta(uint32_t want, uint32_t floor_ms) {
    if (want < floor_ms) {
        want = floor_ms;
    }
    if (want > MODE_C_DELTA_MAX_MS) {
        want = MODE_C_DELTA_MAX_MS;
    }
    return want;
}

static void paint_mask(uint16_t mask, const struct led_rgb c) {
    for (int i = 0; i < LED_PIXEL_COUNT; i++) {
        if (mask & (uint16_t)(1u << i)) {
            led_pixel_set((uint8_t)i, c.r, c.g, c.b);
        }
    }
}

/* ---- rendering ---- */

static void render_step(void) {
    led_pixel_clear();
    if (cur_step >= step_count) {
        led_pixel_update();
        return;
    }
    uint16_t cur = steps[cur_step].mask;
    paint_mask(cur, CUE_BLUE);
    if (cur_step + 1 < step_count) {
        /* A key belonging to both this step and the next stays BLUE: the cue for
         * "press this now" must win over the cue for "press this next". */
        paint_mask((uint16_t)(steps[cur_step + 1].mask & (uint16_t)~cur), CUE_RED);
    }
    led_pixel_update();
}

static void flash_mask(uint16_t mask, const struct led_rgb c, int ms) {
    led_pixel_clear();
    paint_mask(mask, c);
    led_pixel_update();
    k_work_schedule(&restore_work, K_MSEC(ms));
}

/* ---- work handlers ---- */

static void step_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    if (state != MC_RUNNING) return;
    if (pace != MODE_C_PACE_AUTO) return;   /* switched to manual while pending */
    mc_advance();
}

static void advance_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mc_advance();
}

static void restore_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    /* Only restore while parked on a step: mid-advance the advance work owns the
     * next render, and re-drawing the step that was just hit would read as "press
     * this again". */
    if (state == MC_RUNNING) {
        render_step();
    }
}

/* ---- step arming / progression ---- */

/* Arm (or disarm) the auto-advance clock for the step currently on screen. In
 * manual pace this deliberately leaves no timer pending at all: a step waits for
 * the user for as long as it takes. */
static void mc_arm_timer(void) {
    k_work_cancel_delayable(&step_work);
    if (state != MC_RUNNING || pace != MODE_C_PACE_AUTO) {
        return;
    }
    k_work_schedule(&step_work,
                    K_MSEC(clamp_delta(steps[cur_step].delta, MODE_C_AUTO_MIN_MS)));
}

static void mc_arm_step(void) {
    state = MC_RUNNING;
    hit_mask = (uint16_t)(pressed_mask & steps[cur_step].mask);
    render_step();
    mode_c_notify_event(MODE_C_EVT_STEP, chord_size(steps[cur_step].mask),
                        cur_step);
    mc_arm_timer();
}

static void mc_step_hit(void) {
    uint16_t want = steps[cur_step].mask;
    mode_c_notify_event(MODE_C_EVT_HIT, lowest_key(want), cur_step);
    flash_mask(want, CUE_GREEN, 140);
    k_work_cancel_delayable(&step_work);
    state = MC_ADVANCING;
    k_work_schedule(&advance_work,
                    K_MSEC(clamp_delta(steps[cur_step].delta, MODE_C_HIT_GAP_MIN_MS)));
}

static void mc_advance(void) {
    cur_step++;
    if (cur_step >= step_count) {
        state = MC_IDLE;
        led_pixel_clear();
        led_pixel_update();
        effects_player_exit();   /* release the strip back to the user's effects */
        mode_c_notify_event(MODE_C_EVT_DONE, 0, step_count);
        LOG_INF("mode_c: finished %u steps", step_count);
        return;
    }
    mc_arm_step();
}

/* ---- physical key presses ---- */

void mode_c_on_position(uint32_t position, bool pressed) {
    if (position >= LED_PIXEL_COUNT) return;

    uint16_t bit = (uint16_t)(1u << position);
    /* Bookkeeping happens in every state, not just MC_RUNNING: a finger that goes
     * down during the post-HIT gap must be counted when the next step arms. */
    if (pressed) {
        pressed_mask |= bit;
    } else {
        pressed_mask &= (uint16_t)~bit;
    }

    if (state != MC_RUNNING) return;
    if (!pressed) return;
    if (pace != MODE_C_PACE_MANUAL) return;   /* auto: lights only, no judging */

    uint16_t want = steps[cur_step].mask;
    if ((bit & want) == 0) {
        /* Wrong key. Report it and stay put; the keys already accepted for this
         * chord are kept, so one fat finger does not force a full restart. */
        flash_mask(MODE_C_ALL_KEYS, CUE_RED, 220);
        mode_c_notify_event(MODE_C_EVT_MISS, (uint8_t)(position + 1), cur_step);
        return;
    }

    hit_mask |= bit;
    if ((hit_mask & want) == want) {
        mc_step_hit();
    }
}

/* ---- public API ---- */

void mode_c_start(uint16_t note_count) {
    if (step_count == 0) {
        LOG_WRN("mode_c_start: no steps pushed yet");
        return;
    }
    /* Restarting while a session is live drops the old playback state but KEEPS the
     * step table: the App pushes first and starts second, so the table is the score
     * we are about to play. (Clearing it here is mode_c_stop()'s job.) */
    k_work_cancel_delayable(&step_work);
    k_work_cancel_delayable(&advance_work);
    k_work_cancel_delayable(&restore_work);
    cur_step = 0;
    hit_mask = 0;
    pressed_mask = 0;
    effects_player_enter();   /* claim the LED strip, stop the effect tick */
    LOG_INF("mode_c_start: %u steps (requested %u), pace %u", step_count, note_count, pace);
    mc_arm_step();
}

void mode_c_push(uint16_t delta_ms, uint16_t key_mask, uint8_t duration_ms) {
    if (step_count >= MODE_C_MAX_STEPS) {
        LOG_WRN("mode_c_push: buffer full");
        return;
    }
    /* A step must sound at least one key, and no bit above key 15 may be set: an
     * out-of-range bit would be silently ignored by the renderer and the step could
     * then never be satisfied. */
    if (key_mask == 0 || (key_mask & (uint16_t)~MODE_C_ALL_KEYS) != 0) {
        LOG_WRN("mode_c_push: bad mask 0x%04x (need 1..15)", key_mask);
        return;
    }
    steps[step_count].delta    = delta_ms;
    steps[step_count].mask     = key_mask;
    steps[step_count].duration = duration_ms;
    step_count++;
}

void mode_c_tick(uint32_t app_ms) {
    app_ref_ms = app_ms;
    /* Informational only; Mode C timing lives on the keyboard clock. The reference
     * is logged so it can be correlated in a bug report. */
    LOG_DBG("mode_c_tick: app_ref_ms=%u", app_ref_ms);
}

void mode_c_stop(void) {
    k_work_cancel_delayable(&step_work);
    k_work_cancel_delayable(&advance_work);
    k_work_cancel_delayable(&restore_work);
    state = MC_IDLE;
    hit_mask = 0;
    pressed_mask = 0;
    step_count = 0;   /* the table is per-session; the App always PUSHes again */
    led_pixel_clear();
    led_pixel_update();
    effects_player_exit();
    LOG_INF("mode_c_stop");
}

void mode_c_set_pace(uint8_t p) {
    pace = (p == MODE_C_PACE_AUTO) ? MODE_C_PACE_AUTO : MODE_C_PACE_MANUAL;
    LOG_INF("mode_c: pace -> %u", pace);
    if (state == MC_RUNNING) {
        /* Re-arm the step on screen so the switch takes effect immediately and in
         * both directions: auto -> manual cancels the pending auto-advance and
         * starts waiting for the user; manual -> auto starts that step's clock now
         * rather than on the next hit. */
        mc_arm_step();
    }
}

uint8_t mode_c_get_pace(void) {
    return pace;
}

bool mode_c_is_active(void) {
    return state == MC_RUNNING || state == MC_ADVANCING;
}

uint16_t mode_c_current_step(void) {
    return cur_step;
}

uint16_t mode_c_step_count(void) {
    return step_count;
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
    k_work_init_delayable(&step_work, step_work_fn);
    k_work_init_delayable(&advance_work, advance_work_fn);
    k_work_init_delayable(&restore_work, restore_work_fn);
    state = MC_IDLE;
    step_count = 0;
    cur_step = 0;
    /* Touch the build tag so the linker keeps it - see the comment on mc_build_tag. */
    mc_build_tag_sink = *mc_build_tag_probe;
    return 0;
}
