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
 *   index 0  - RED,   held 3.0 s   (long, so you cannot miss it)
 *   index 1  - GREEN, held 0.9 s
 *   index 2  - BLUE,  held 0.9 s
 *   index 3  - WHITE, held 0.9 s   (TEMPORARY: 4-pixel bring-up board)
 *   then 1.5 s all-off, and the 4-step cycle repeats
 *
 * Press any key to stop; the strip is handed back to the normal effect
 * (CONFIG_ZMK_RGB_PLAYER_DEFAULT_EFFECT) so the board stays usable.
 *
 * HOW TO READ IT - three outcomes, three different faults
 * -------------------------------------------------------
 * A) At the RED step exactly ONE physical LED lights, and as the cycle
 *    advances the lit LED walks along the strip.
 *    -> Healthy daisy chain. The strip was never the problem; stop here.
 *
 * B) At the RED step ALL FOUR light up together (all red), and every later
 *    step also lights all four in that step's colour.
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
 * Note on colours: values are capped at 180/255 so that outcome B (all LEDs
 * lit at once) stays well inside the supply budget - and at 4 pixels the cap
 * is generous anyway: even all four white is well under 100 mA on the
 * ST-1209RGB (5 mA/channel class), versus hundreds of mA with WS2812B 5050.
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

K_THREAD_STACK_DEFINE(cp_stack, CP_STACK_SIZE);
static struct k_thread cp_thread;
static volatile bool cp_running;

/* One colour per data index, all components <= 180 (see file header).
 * TEMPORARY: exactly LED_PIXEL_COUNT (=4) rows - this array is sized from
 * LED_PIXEL_COUNT, so adding rows here without restoring the count breaks the
 * build. Restore all 15 rows together with chain-length / LED_PIXEL_COUNT. */
static const uint8_t cp_pal[LED_PIXEL_COUNT][3] = {
    {180,   0,   0},   /*  0 red        */
    {  0, 180,   0},   /*  1 green      */
    {  0,   0, 180},   /*  2 blue       */
    {180, 180, 180},   /*  3 white      */
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

static void cp_thread_fn(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    k_sleep(K_MSEC(CP_START_DELAY_MS));

    while (cp_running) {
        for (int i = 0; i < LED_PIXEL_COUNT; i++) {
            if (!cp_running) break;
            cp_show_index(i);
            k_sleep(K_MSEC(i == 0 ? CP_HEAD_HOLD_MS : CP_STEP_HOLD_MS));
            cp_all_off();
            k_sleep(K_MSEC(CP_GAP_MS));
        }
        if (!cp_running) break;
        k_sleep(K_MSEC(CP_CYCLE_PAUSE_MS));
    }

    cp_all_off();

    /* Hand the strip back. Without this the board would stay dark after the
     * first key press, which looks exactly like the fault being diagnosed. */
    effects_set_active((rgb_effect_t)CONFIG_ZMK_RGB_PLAYER_DEFAULT_EFFECT);
}

/* Any key press ends the inspection and restores normal operation. Runs in
 * the input path, so it only sets a flag - no SPI, no logging. */
static int cp_position_cb(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (ev->state) {
        cp_running = false;
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
