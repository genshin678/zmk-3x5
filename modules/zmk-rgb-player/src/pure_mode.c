/*
 * pure_mode.c - 纯净弹琴模式 (see pure_mode.h for the design)
 *
 * Gesture    : hold Y + N + P + /   (positions 0, 4, 10, 14 - the four corners)
 * Feedback   : K (position 7) blinks BLUE twice on enter, RED twice on leave
 * Enforcement: keymap switches to layer 1, where every function combo is
 *              inactive (`layers = <0>` in the combos node)
 *
 * THREE THINGS THIS FILE HAS TO GET RIGHT
 * ---------------------------------------
 * 1. SEEING THE KEYS AT ALL. The four gesture keys are also members of the
 *    existing function combos (Y+N effect, Y+P brightness, P+N DFU). ZMK's
 *    combo engine CAPTURES the position events of its member keys and stops
 *    event dispatch at that point (event_manager.c returns 0 on
 *    ZMK_EV_EVENT_CAPTURED), so a listener registered AFTER combo.c would never
 *    see Y, N or P at all - the gesture could not be detected. Module sources
 *    are added to the `app` target before ZMK's own sources (our CMakeLists is
 *    processed inside find_package(Zephyr), zmk/app adds its sources after),
 *    so this subscription sits earlier in .event_subscription than combo.c and
 *    runs first. The same is already true of effects.c, which is why its
 *    per-key ripple sees every press. Do NOT move this listener behind a
 *    conditional that could reorder it.
 *
 * 2. THE GESTURE'S OWN SIDE EFFECTS. Y+N and Y+P fire *before* the fourth key
 *    lands, so entering pure mode would otherwise leave the effect cycled and
 *    the brightness nudged. The light state is therefore snapshotted on the
 *    FIRST corner key and restored when the gesture completes. Restoring (not
 *    blocking) is the only option that keeps those combos usable for their real
 *    purpose: at two keys down, "brightness chord" and "pure-mode gesture" are
 *    literally the same input.
 *
 * 3. NOT FIGHTING THE RENDERER. The blink does not stop the render tick and
 *    does not repaint the strip: it sets a one-pixel override inside
 *    led_pixel.c, so whichever renderer owns the strip at that moment (ambient
 *    effect, Mode C cue frames, the score player) keeps painting and the
 *    override is applied on top of every frame it pushes. That is also why the
 *    blink still shows up mid-song.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <zmk_rgbeffect/pure_mode.h>
#include <zmk_rgbeffect/effects.h>
#include <zmk_rgbeffect/led_pixel.h>
#include <zmk_rgbeffect/mode_c.h>
#include <zmk_rgbeffect/rgb_control.h>

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

/* --- the gesture --------------------------------------------------------- */

#define GPOS_Y 0u
#define GPOS_P 4u
#define GPOS_N 10u
#define GPOS_SLASH 14u

#define GESTURE_BIT(pos) ((uint16_t)(1u << (pos)))
#define GESTURE_MASK                                                                               \
    (GESTURE_BIT(GPOS_Y) | GESTURE_BIT(GPOS_P) | GESTURE_BIT(GPOS_N) | GESTURE_BIT(GPOS_SLASH))

/* All four have to be down inside this window, measured from the first one.
 * Generous on purpose: four corners spread across a 5x3 pad cannot be hit
 * inside the combos' 80 ms, and there is no combo engine here whose timeout
 * would also delay a lone key. 800 ms still rules out "resting a hand flat".
 * Nothing fires on the fourth key alone - the window is check-and-forget, so a
 * slow spread does nothing at all rather than half a gesture. */
#define GESTURE_WINDOW_MS 800

/* --- the blink ----------------------------------------------------------- */

#define BLINK_ON_MS 130
#define BLINK_OFF_MS 110
/* on, off, on, off: "闪两下" is two flashes, i.e. two ON phases. */
#define BLINK_PHASES 4
/* Vivid on purpose - this is one pixel for half a second, nowhere near the
 * 15-pixel current step the MC_CUE_LEVEL ceiling exists for. */
#define BLINK_BLUE_R 0
#define BLINK_BLUE_G 88
#define BLINK_BLUE_B 255
#define BLINK_RED_R 255
#define BLINK_RED_G 0
#define BLINK_RED_B 0

static bool pure_active;

/* Corner keys currently down, as a bit per POSITION (only the four above). */
static uint16_t gesture_down;
static int64_t gesture_first_ms;
/* False once the gesture has fired (or was too slow) until every corner is
 * released - otherwise holding the four keys would toggle the mode repeatedly
 * on every re-raised event. */
static bool gesture_armed = true;

/* Light state captured before the gesture's member combos could change it. */
static bool snap_valid;
static uint8_t snap_brightness;
static uint16_t snap_hue;
static rgb_effect_t snap_effect;

static struct k_work_delayable blink_work;
static uint8_t blink_left; /* phases still to show: 4 = on, 3 = off, 2 = on, 1 = off */
static uint8_t blink_r, blink_g, blink_b;

/* The light restore is a WORK ITEM, not inline in the position listener.
 * effects_set_active(RGB_EFFECT_OFF) joins the render thread (up to one 10 ms
 * tick) and any effect change costs a 660 us SPI frame; the listener runs
 * inside the input path, on the key press that completes the gesture, i.e. at
 * the worst possible moment to add milliseconds. Deferring costs nothing the
 * user can see - the blink itself is already deferred by 10 ms. */
static void restore_work_fn(struct k_work *work);

static void blink_phase_fn(struct k_work *work) {
    ARG_UNUSED(work);

    if (blink_left == 0) {
        return;
    }

    /* 4 -> ON, 3 -> OFF, 2 -> ON, 1 -> OFF. */
    const bool on = ((blink_left & 1u) == 0);

    if (on) {
        led_pixel_set_override((uint8_t)PURE_MODE_BLINK_KEY, blink_r, blink_g, blink_b);
    } else {
        led_pixel_clear_override();
    }
    /* Push immediately: with the strip effect OFF there is no render tick, and
     * even with one running the phase change must not wait for the next frame. */
    led_pixel_update();

    blink_left--;
    if (blink_left) {
        k_work_reschedule(&blink_work, K_MSEC(on ? BLINK_ON_MS : BLINK_OFF_MS));
    }
}

static void blink(uint8_t r, uint8_t g, uint8_t b) {
    blink_r = r;
    blink_g = g;
    blink_b = b;
    blink_left = BLINK_PHASES;
    /* Deferred: this may be called from the input path, and the first frame is
     * a synchronous ~660 us SPI burst. 10 ms is invisible to the user. */
    k_work_reschedule(&blink_work, K_MSEC(10));
}

/* --- mode change --------------------------------------------------------- */

/* Undo whatever the gesture's own combos did while the four keys were going
 * down. Y+P walks brightness up every 100 ms and Y+N cycles the effect; both
 * have already run by the time the fourth corner lands. */
static void restore_pre_gesture_lights(void) {
    rgb_control_bri_loop_stop();

    if (!snap_valid) {
        return;
    }
    snap_valid = false;

    rgb_control_set_brightness(snap_brightness);
    rgb_control_set_hue(snap_hue);

    if (effects_get_active() != snap_effect) {
        if (mode_c_is_active()) {
            /* The player owns the strip; applying it now would start the
             * ambient tick on top of the cue frames. */
            effects_set_active_deferred(snap_effect);
        } else {
            effects_set_active(snap_effect);
        }
    }
}

static struct k_work restore_item;

static void restore_work_fn(struct k_work *work) {
    ARG_UNUSED(work);
    restore_pre_gesture_lights();
}

void pure_mode_toggle(void) {
    pure_active = !pure_active;

    if (pure_active) {
        /* Stop the repeat first - it is two register writes and must not keep
         * walking brightness while the rest is being restored - then hand the
         * (potentially blocking) repaint to the workqueue. */
        rgb_control_bri_loop_stop();
        k_work_submit(&restore_item);
        zmk_keymap_layer_to((zmk_keymap_layer_id_t)PURE_MODE_LAYER);
        LOG_INF("pure mode ON (layer %u)", (unsigned)PURE_MODE_LAYER);
        blink(BLINK_BLUE_R, BLINK_BLUE_G, BLINK_BLUE_B);
    } else {
        zmk_keymap_layer_to(zmk_keymap_layer_default());
        LOG_INF("pure mode OFF");
        blink(BLINK_RED_R, BLINK_RED_G, BLINK_RED_B);
    }
}

bool pure_mode_is_active(void) {
    return pure_active;
}

/* --- gesture detection --------------------------------------------------- */

static int pure_mode_position_cb(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->position >= 16u) {
        return ZMK_EV_EVENT_BUBBLE; /* not a key of this pad (sensor / virtual) */
    }

    const uint16_t bit = GESTURE_BIT(ev->position);
    if ((GESTURE_MASK & bit) == 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* k_uptime_get() rather than ev->timestamp: the timestamp is supplied by the
     * event source and re-raised events carry the original one. Wall clock at
     * handling time is what "were these four pressed together" means here, and
     * this runs synchronously in the input path. */
    const int64_t now = (int64_t)k_uptime_get();

    if (ev->state) {
        if (gesture_down == 0) {
            gesture_first_ms = now;
            /* First corner of a fresh run: park the light state before the
             * combos on this key can touch it. */
            if (!pure_active && !snap_valid) {
                snap_brightness = rgb_control_get_brightness();
                snap_hue = rgb_control_get_hue();
                snap_effect = effects_get_active();
                snap_valid = true;
            }
        }
        gesture_down |= bit;

        if (gesture_armed && (gesture_down & GESTURE_MASK) == GESTURE_MASK) {
            /* One shot either way. A spread slower than the window is treated
             * as "not a gesture" and must not fire later when the last finger
             * happens to land. */
            gesture_armed = false;
            if (now - gesture_first_ms <= GESTURE_WINDOW_MS) {
                pure_mode_toggle();
            }
        }
    } else {
        gesture_down &= (uint16_t)~bit;
        if ((gesture_down & GESTURE_MASK) == 0) {
            gesture_armed = true;
            if (!pure_active) {
                /* Ordinary corner use (brightness / effect / DFU chord): the
                 * user MEANT those changes, so drop the snapshot instead of
                 * restoring it later. */
                snap_valid = false;
            }
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(pure_mode_position, pure_mode_position_cb);
ZMK_SUBSCRIPTION(pure_mode_position, zmk_position_state_changed);

void pure_mode_init(void) {
    pure_active = false;
    gesture_down = 0;
    gesture_armed = true;
    snap_valid = false;
    k_work_init_delayable(&blink_work, blink_phase_fn);
    k_work_init(&restore_item, restore_work_fn);
}
