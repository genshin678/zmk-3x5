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
#include <zmk/hid.h>
#include <zmk/endpoints.h>
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif
#include <zmk_rgbeffect/mode_c.h>
#include <zmk_rgbeffect/effects.h>
#include <zmk_rgbeffect/led_pixel.h>
#include <zmk_rgbeffect/player.h>
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
static struct k_work_delayable release_work;    /* AUTO: lift the emitted HID keys */
static struct k_work_delayable hid_retry_work;   /* re-send a report the link refused */
static uint16_t sent_mask = 0;               /* keys the HOST was last TOLD are down */
static uint16_t want_mask = 0;               /* keys the host should be holding now */
static uint8_t  hid_fail_streak = 0;         /* consecutive undeliverable reports */
static uint8_t  hid_fail_total  = 0;         /* saturating, for the BE04 diag field */
static bool     hid_warned = false;          /* one HID_FAIL event per outage */

static void mc_advance(void);
static void mc_hid_flush(void);

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
    "MCV6/LINK+DIAG";
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

/* ---- auto pace: emit real keystrokes ----
 *
 * AUTO used to animate the strip and nothing else. The lights walked the score while the
 * host received no input at all, so the mode read as dead. Its whole purpose is for the
 * keyboard to PLAY the instrument in the game, and that means synthesising HID reports.
 *
 * One step is emitted as ONE report carrying every key of its mask: a chord has to arrive
 * as a chord, or the game's note detector hears an arpeggio instead. The keys are lifted by
 * a delayed work rather than a sleep, so the system workqueue - which also drives the LED
 * effects - is never blocked.
 *
 * The usage IDs come from player.c via player_key_scancodes(): one copy, so the two
 * playback paths can never disagree about which code is "Y".
 *
 * MANUAL never emits anything - there the human is the player.
 *
 * Every way out of playback (STOP, DONE, switching back to manual, a fresh START) lifts
 * whatever this engine holds, so a key can never be left stuck down on the host. */
#define MC_TAP_MIN_MS     50u   /* shortest hold the game still registers */
#define MC_TAP_MAX_MS    200u   /* longest: keeps dense passages from smearing */
#define MC_TAP_DEFAULT_MS 80u   /* used when a PUSH carries duration 0 */

/* A report the link refuses is retried this many times, this far apart. Both are
 * tuned against the shortest step the firmware will ever arm (MODE_C_AUTO_MIN_MS =
 * 60 ms): the whole retry budget has to fit inside one step, or a late retry would
 * fight the next chord. */
#define MC_HID_RETRY_MAX 8u
#define MC_HID_RETRY_MS  5u

/* Apply the difference between what the host should be holding and what it was last
 * successfully told, on the global ZMK keyboard report. Only the DIFF is touched: a
 * press on a key that is already in the report would take a second slot of the
 * 6-key report, and four chords later the report is full and presses start failing. */
static void mc_hid_apply(uint16_t want) {
    const uint8_t *sc = player_key_scancodes();
    for (uint8_t k = 0; k < PLAYER_KEY_COUNT; k++) {
        uint16_t bit = (uint16_t)(1u << k);
        if ((want & bit) == (sent_mask & bit)) {
            continue;
        }
        if (want & bit) {
            zmk_hid_keyboard_press(sc[k]);
        } else {
            zmk_hid_keyboard_release(sc[k]);
        }
    }
}

/* ---- HID report delivery ----
 *
 * zmk_endpoints_send_report() is NOT fire-and-forget: it returns a negative errno when
 * the report never left the board.
 *   -ENODEV  the selected endpoint is not ready (USB not enumerated or suspended, the
 *            bonded BLE peer not connected)
 *   -EBUSY   the endpoint was still busy with the previous report, so THIS one was
 *            thrown away (ZMK's USB path writes with K_NO_WAIT behind a 30 ms semaphore
 *            take whose result it ignores)
 * There is no retry queue behind it. A dropped report is gone.
 *
 * That matters more here than anywhere else in ZMK, because a HID report is STATE, not
 * a delta: the host displays whatever the last report that actually ARRIVED said. So
 * losing the release report does not lose a note - it leaves the whole chord held down
 * on the host, forever, because sent_mask used to be zeroed regardless of whether the
 * report went out. This engine then believed the keys were already up and never sent
 * another release. In a music game the symptom is "the keyboard froze": the instrument
 * keeps holding the note and every later chord is heard as an extension of that hold,
 * so the score stops advancing while the strip keeps walking.
 *
 * So sent_mask only ever records what the host was TOLD, and a failed report is
 * retried. A retry re-sends the report WITHOUT re-applying the key diff (see
 * mc_hid_apply): the report is already the state we want, it just never arrived.
 *
 * The failure is also surfaced - a saturating total in BE04 plus a one-shot HID_FAIL
 * event - because with CONFIG_LOG=n "nothing happened" is not a diagnosis. */
static void mc_hid_retry_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mc_hid_flush();
}

/* Push the state this engine currently wants out to the host, retrying on failure. */
static void mc_hid_flush(void) {
    int err = zmk_endpoints_send_report(0x07);   /* HID keyboard usage page */
    if (err == 0) {
        sent_mask = want_mask;
        hid_fail_streak = 0;
        hid_warned = false;
        return;
    }
    hid_fail_streak++;
    if (hid_fail_total < 0xFFu) {
        hid_fail_total++;
    }
    if (!hid_warned) {
        /* One event per outage, not one per report: the App shows this verbatim, and
         * 990 identical lines would be noise. */
        hid_warned = true;
        mode_c_notify_event(MODE_C_EVT_HID_FAIL, hid_fail_streak, hid_fail_total);
    }
    if (hid_fail_streak <= MC_HID_RETRY_MAX) {
        /* reschedule, NOT schedule: this runs from hid_retry_work's own handler on a
         * retry, and k_work_schedule() would refuse (-EALREADY) and keep the stale
         * deadline. Same reason step_work is re-armed this way - see mc_arm_timer(). */
        k_work_reschedule(&hid_retry_work, K_MSEC(MC_HID_RETRY_MS));
    }
}

/* Set the state the host should hold and push it out. */
static void mc_hid_set(uint16_t want) {
    want_mask = want;
    mc_hid_apply(want);
    mc_hid_flush();
}

/* Lift everything this engine holds. A FAILED release deliberately leaves sent_mask
 * set, so the next flush - or the retry work, or STOP - still knows the host is
 * holding keys and can undo it. */
static void mc_hid_release_all(void) {
    if (want_mask == 0 && sent_mask == 0) {
        return;
    }
    mc_hid_set(0);
}

static void release_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    mc_hid_release_all();
}

static void mc_hid_tap(uint16_t mask, uint8_t duration_ms) {
    /* Lift the previous step first: were a key from step N still down when step N+1's
     * chord arrived, the host would see a chord that was never written. */
    mc_hid_release_all();

    uint16_t sent = 0;
    for (uint8_t k = 0; k < PLAYER_KEY_COUNT; k++) {
        if (mask & (uint16_t)(1u << k)) {
            sent |= (uint16_t)(1u << k);
        }
    }
    if (sent == 0) {
        return;
    }

    uint32_t hold = duration_ms ? duration_ms : MC_TAP_DEFAULT_MS;
    if (hold < MC_TAP_MIN_MS) {
        hold = MC_TAP_MIN_MS;
    }
    if (hold > MC_TAP_MAX_MS) {
        hold = MC_TAP_MAX_MS;
    }
    /* Arm the release BEFORE the press report. If the press needs its retry budget the
     * release still happens on schedule, and a chord can never stick because its own
     * press was the slow part. */
    k_work_reschedule(&release_work, K_MSEC(hold));
    mc_hid_set(sent);
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
    /* reschedule, not schedule: a second flash landing inside the first one's window
     * would otherwise be ignored (k_work_schedule() no-ops on an already-delayed item
     * and keeps the OLD deadline), cutting the newer flash short. */
    k_work_reschedule(&restore_work, K_MSEC(ms));
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

/* Arm the auto-advance clock for the step currently on screen. In manual pace this
 * arms nothing: a step waits for the user for as long as it takes. */
static void mc_arm_timer(void) {
    if (state != MC_RUNNING || pace != MODE_C_PACE_AUTO) {
        /* Deliberately does NOT cancel. A timer left over from a previous arm is
         * harmless - step_work_fn re-checks state and pace before doing anything -
         * whereas cancelling here is what broke auto play; see below. The two places
         * that genuinely must kill the clock (mode_c_start, mode_c_stop) cancel it
         * explicitly, and neither of them runs inside a work handler. */
        return;
    }
    /* k_work_reschedule, NOT k_work_cancel_delayable() + k_work_schedule().
     *
     * This function runs from inside step_work's OWN handler:
     *     step_work_fn -> mc_advance -> mc_arm_step -> mc_arm_timer.
     * Cancelling a work item from its own handler does not merely fail: it sets
     * K_WORK_CANCELING_BIT on that item. The k_work_schedule() that used to follow was
     * then silently refused, because its admission test is
     *     (k_work_busy_get() & ~K_WORK_RUNNING) == 0
     * and CANCELING makes that non-zero. No deadline was armed and nothing was logged,
     * so AUTO advanced exactly once (step 0 -> step 1) and then froze forever: the
     * strip lit a cue and never moved again.
     *
     * reschedule() is unschedule + schedule and never touches CANCELING, so it is the
     * only safe way to re-arm an item from within that item's handler. player.c's 5 ms
     * playback tick and rgb_control.c's bri/hue loops are periodic for exactly this
     * reason: they re-arm with k_work_schedule() but never cancel first. */
    k_work_reschedule(&step_work,
                      K_MSEC(clamp_delta(steps[cur_step].delta, MODE_C_AUTO_MIN_MS)));
}

static void mc_arm_step(void) {
    state = MC_RUNNING;
    hit_mask = (uint16_t)(pressed_mask & steps[cur_step].mask);
    render_step();
    mode_c_notify_event(MODE_C_EVT_STEP, chord_size(steps[cur_step].mask),
                        cur_step);
    if (pace == MODE_C_PACE_AUTO) {
        /* The keyboard is the player in this pace: sound the chord for real. */
        mc_hid_tap(steps[cur_step].mask, steps[cur_step].duration);
    }
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
        mc_hid_release_all();
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
    k_work_cancel_delayable(&release_work);
    hid_warned = false;       /* a fresh run reports its own failures */
    mc_hid_release_all();     /* a fresh start must not inherit a held key */
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
    k_work_cancel_delayable(&release_work);
    /* Deliberately NOT cancelling hid_retry_work: if the release below does not get
     * through, that work is the only thing left that will ever lift the chord off the
     * host, and a stuck chord is indistinguishable from a frozen keyboard. */
    hid_warned = false;
    mc_hid_release_all();
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
    if (pace != MODE_C_PACE_AUTO) {
        /* Handing control back to the human: lift anything the auto pace is holding
         * first, or the first thing they hear is a stuck note. */
        k_work_cancel_delayable(&release_work);
        mc_hid_release_all();
    }
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

/* HID delivery counters, mirrored into BE04 so the App can show them. Without this an
 * undeliverable report is invisible: the strip walks the score while the host receives
 * nothing at all, which reads as "the keyboard froze". */
/* ---- link diagnostics (BE04 byte 13/14, fw_flags bit4) ----
 *
 * ZMK v0.3 numbers enum zmk_transport as {USB=0, BLE=1}, while ZMK main uses
 * {NONE=0, USB=1, BLE=2}. Our wire values are therefore defined here and the
 * comparison is done on the SYMBOLS, so a ZMK upgrade that renumbers the enum
 * cannot silently turn USB into BLE on the App's screen.
 */
uint8_t mode_c_link_endpoint(void) {
    struct zmk_endpoint_instance ep = zmk_endpoints_selected();
    if (ep.transport == ZMK_TRANSPORT_USB) {
        return 1;
    }
    if (ep.transport == ZMK_TRANSPORT_BLE) {
        return 2;
    }
    return 0;
}

uint8_t mode_c_link_usb_state(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    switch (zmk_usb_get_conn_state()) {
    case ZMK_USB_CONN_NONE:
        return 0;   /* nothing on the connector */
    case ZMK_USB_CONN_POWERED:
        return 1;   /* charging, but NOT enumerated as a keyboard */
    case ZMK_USB_CONN_HID:
        return 2;   /* enumerated: reports can go out over the wire */
    default:
        return 0xFF;
    }
#else
    return 0xFF;    /* no USB support in this image */
#endif
}

uint8_t mode_c_hid_fail_total(void) {
    return hid_fail_total;
}

uint8_t mode_c_hid_fail_streak(void) {
    return hid_fail_streak;
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
    k_work_init_delayable(&release_work, release_work_fn);
    k_work_init_delayable(&hid_retry_work, mc_hid_retry_fn);
    state = MC_IDLE;
    step_count = 0;
    cur_step = 0;
    /* Touch the build tag so the linker keeps it - see the comment on mc_build_tag. */
    mc_build_tag_sink = *mc_build_tag_probe;
    return 0;
}
