/*
 * chainprobe.c - WS2812 daisy-chain inspector (TEMPORARY diagnostic)
 *
 * WHY THIS EXISTS
 * ---------------
 * A multimeter cannot tell you the topology of a WS2812 string. Continuity
 * beeps through ESD clamping diodes (DIN pad -> diode -> common VDD rail ->
 * another pad's diode -> its DIN), through a live supply, and through the
 * 0.5 mm-pitch pins of a TXS0102 that you cannot reliably probe one at a
 * time. Two readings taken the same afternoon contradicted each other, which
 * is the signature of a bad instrument for the job - not of a bad board.
 *
 * This module answers the question with the strip itself, which is the only
 * observer that sees the data actually arriving.
 *
 * WHAT IT DOES
 * ------------
 * Lights ONE DATA INDEX at a time, in order, each in its own colour:
 *
 *   index 0  - RED,    held 3.0 s   (long, so you cannot miss it)
 *   index 1  - orange, held 0.9 s
 *   index 2  - yellow, held 0.9 s
 *   ...        (a 15-step hue ramp; each data index gets its own colour)
 *   index 14 - pink,   held 0.9 s
 *   then 1.5 s all-off, and the 15-step cycle repeats
 *
 * HOLD any key for 0.6 s to stop; the strip is then handed back to the normal
 * effect (CONFIG_ZMK_RGB_PLAYER_DEFAULT_EFFECT) so the board stays usable.
 *
 * Two guards keep this inspection alive, because on this board it kept losing
 * a race it could not see:
 *
 *   1. A 3 s grace window after boot during which EVERY key event is dropped.
 *      A floating matrix input (or a gated rail still coming up) reports a
 *      keypress at boot; the earlier code acted on it while the probe was
 *      still inside its 400 ms settle sleep, so the probe exited before it
 *      lit a single LED and the only thing ever seen on the strip was the
 *      fallback rainbow effect.
 *   2. A deliberate HOLD of 0.6 s to stop, so neither a phantom event at
 *      boot nor a key you brush while probing can end the run.
 *
 * HOW TO READ IT - three outcomes, three different faults
 * -------------------------------------------------------
 * A) At the RED step exactly ONE physical LED lights, and as the cycle
 *    advances the lit LED walks along the strip.
 *    -> Healthy daisy chain. The strip was never the problem; stop here.
 *
 * B) At the RED step ALL FIFTEEN light up together (all red), and every later
 *    step also lights all fifteen in that step's colour.
 *    -> BUS topology, not a chain: every DIN is tied to the same net. Every
 *       LED receives identical data, so the firmware can never address them
 *       individually. This is a LAYOUT/BOARD fault. The fix is to cut the
 *       common net and wire DOUT(n) -> DIN(n+1) - NOT to add a jumper.
 *       (A TXS0102, which drives its high level through 10 kohm pull-ups,
 *       cannot drive 15 inputs' worth of capacitance, which is why a bus
 *       shows up as "only the closest LED decodes correctly".)
 *
 * C) At the RED step exactly ONE physical LED lights, and NOTHING lights at
 *    any later step.
 *    -> That one LED is the chain head and data does not reach the second
 *       LED. Break is between its DOUT and the next DIN. Fix: fly a wire
 *       from the last LED that DOES respond to the next one's DIN.
 *    -> Note which physical position the red one occupied: that is your
 *       data index 0, and it is where the chain starts counting from.
 *
 * Note on colours: values are capped at 200/255 so that outcome B (all LEDs
 * lit at once) stays well inside the supply budget: even all fifteen white is
 * ~225 mA on the ST-1209RGB (5 mA/channel class), within the MT3608 5V rail,
 * versus hundreds of mA with WS2812B 5050.
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_ZMK_RGB_PLAYER_CHAINPROBE)

#include <zmk_rgbeffect/led_pixel.h>
#include <zmk_rgbeffect/effects.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

/* Render thread runs at 12; this inspector is equally unimportant. */
#define CP_THREAD_PRIORITY  12
#define CP_STACK_SIZE       1024

#define CP_START_DELAY_MS   400     /* let the led_strip device settle   */
#define CP_HEAD_HOLD_MS     3000    /* index 0: long, it is the key step */
#define CP_STEP_HOLD_MS     900
#define CP_GAP_MS           250
#define CP_CYCLE_PAUSE_MS   1500
#define CP_SLICE_MS         100     /* sleep granularity, so a hold is seen  */
#define CP_EXIT_GRACE_MS    3000    /* drop ALL key events for this long     */
#define CP_EXIT_HOLD_MS     600     /* a key must be HELD this long to stop  */

K_THREAD_STACK_DEFINE(cp_stack, CP_STACK_SIZE);
static struct k_thread cp_thread;
static volatile bool cp_running;

/* Exit-gesture state. Written on the input path, read by the render thread.
 * uint32_t and not int64_t: a 64-bit store is two 32-bit stores on Cortex-M4
 * and can tear, which would hand the reader a garbage timestamp and stop the
 * run instantly. Unsigned subtraction wraps cleanly, so this is safe. */
static volatile bool     cp_armed;          /* false during the grace window */
static volatile bool     cp_key_down;
static volatile uint32_t cp_key_down_ts;

/* One colour per data index, all components <= 200 (see file header). This
 * array is sized from LED_PIXEL_COUNT (=15), so it must hold exactly 15 rows.
 * A hue ramp makes the walk along the strip obvious: each physical LED lights
 * in its own colour in turn. Keep the row count equal to LED_PIXEL_COUNT. */
static const uint8_t cp_pal[LED_PIXEL_COUNT][3] = {
    {200,   0,   0},   /*  0 red     */
    {200,  80,   0},   /*  1 orange  */
    {200, 160,   0},   /*  2 yellow  */
    {160, 200,   0},   /*  3 lime    */
    { 80, 200,   0},   /*  4 green   */
    {  0, 200,   0},   /*  5 green   */
    {  0, 200,  80},   /*  6 teal    */
    {  0, 200, 160},   /*  7 cyan    */
    {  0, 160, 200},   /*  8 sky     */
    {  0,  80, 200},   /*  9 blue    */
    {  0,   0, 200},   /* 10 blue    */
    { 80,   0, 200},   /* 11 violet  */
    {160,   0, 200},   /* 12 purple  */
    {200,   0, 160},   /* 13 magenta */
    {200,   0,  80},   /* 14 pink    */
};

static void cp_all_off(void) {
    led_pixel_clear();
    led_pixel_update();
}

static void cp_show_index(int idx) {
    led_pixel_clear();
    led_pixel_set((uint8_t)idx, cp_pal[idx][0], cp_pal[idx][1], cp_pal[idx][2]);
    led_pixel_update();
}

/* The exit gesture: a key held for CP_EXIT_HOLD_MS, honoured only after the
 * grace window has closed. Touches no peripheral. */
static bool cp_exit_requested(void) {
    if (!cp_armed || !cp_key_down) return false;
    return ((uint32_t)k_uptime_get() - cp_key_down_ts) >= CP_EXIT_HOLD_MS;
}

/* Sleep in slices rather than one long block. A hold is then noticed within
 * CP_SLICE_MS instead of only after the whole step, while the step timings
 * stay exact because the slices still add up to `ms`. Returns false when the
 * run must stop. */
static bool cp_hold_ms(uint32_t ms) {
    while (ms > 0) {
        uint32_t slice = (ms > CP_SLICE_MS) ? CP_SLICE_MS : ms;
        k_sleep(K_MSEC(slice));
        ms -= slice;
        if (!cp_running || cp_exit_requested()) {
            return false;
        }
    }
    return true;
}

static void cp_thread_fn(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    /* Settle the strip AND sit out the grace window. cp_armed stays false for
     * this whole period, so cp_position_cb drops every key event and nothing
     * can end the run before the first LED has lit. */
    (void)cp_hold_ms(CP_START_DELAY_MS + CP_EXIT_GRACE_MS);
    cp_armed = true;

    while (cp_running) {
        bool keep_going = true;

        for (int i = 0; i < LED_PIXEL_COUNT && keep_going; i++) {
            cp_show_index(i);
            keep_going = cp_hold_ms(i == 0 ? CP_HEAD_HOLD_MS : CP_STEP_HOLD_MS);
            if (!keep_going) break;
            cp_all_off();
            keep_going = cp_hold_ms(CP_GAP_MS);
        }

        if (!keep_going) break;
        if (!cp_hold_ms(CP_CYCLE_PAUSE_MS)) break;
    }

    cp_armed = false;
    cp_all_off();

    /* Hand the strip back. Without this the board would stay dark after the
     * first key press, which looks exactly like the fault being diagnosed. */
    effects_set_active((rgb_effect_t)CONFIG_ZMK_RGB_PLAYER_DEFAULT_EFFECT);
}

/* Tracks the exit gesture. Runs on the input path, so it only records state -
 * no SPI, no logging, no blocking. */
static int cp_position_cb(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* During the grace window every event is dropped. That is the whole point:
     * whatever the matrix reports at boot must not be able to end the run, and
     * dropping it also means a key that reads as held from power-on never
     * satisfies the hold gesture later. */
    if (!cp_armed) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->state) {
        if (!cp_key_down) {
            cp_key_down = true;
            cp_key_down_ts = (uint32_t)k_uptime_get();
        }
    } else {
        cp_key_down = false;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(chainprobe, cp_position_cb);
ZMK_SUBSCRIPTION(chainprobe, zmk_position_state_changed);

void chainprobe_init(void) {
    cp_running = true;
    k_thread_create(&cp_thread, cp_stack,
                    K_THREAD_STACK_SIZEOF(cp_stack),
                    cp_thread_fn, NULL, NULL, NULL,
                    CP_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&cp_thread, "chainprobe");
}

#else

void chainprobe_init(void) { }

#endif /* CONFIG_ZMK_RGB_PLAYER_CHAINPROBE */
