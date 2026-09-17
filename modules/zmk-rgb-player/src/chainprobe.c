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
 * Press any key to stop; the strip is handed back to the normal effect
 * (CONFIG_ZMK_RGB_PLAYER_DEFAULT_EFFECT) so the board stays usable.
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

K_THREAD_STACK_DEFINE(cp_stack, CP_STACK_SIZE);
static struct k_thread cp_thread;
static volatile bool cp_running;

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
