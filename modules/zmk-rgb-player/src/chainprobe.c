/*
 * chainprobe.c - WS2812 daisy-chain inspector (TEMPORARY diagnostic)
 *
 * WHY THIS EXISTS
 * ---------------
 * A multimeter cannot tell you the topology of a WS2812 string. Continuity
 * beeps through ESD clamping diodes (DIN pad -> diode -> common VDD rail ->
 * another pad's diode -> its DIN), through a live supply, and through the
 * 0.5 mm-pitch pins of a level shifter you cannot reliably probe one at a
 * time. Two readings taken the same afternoon contradicted each other, which
 * is the signature of a bad instrument for the job - not of a bad board.
 *
 * This module answers the question with the strip itself, which is the only
 * observer that sees the data actually arriving.
 *
 * WHAT IT DOES - two phases, then it repeats forever
 * --------------------------------------------------
 * PHASE 1 - THE SPLIT FRAME, held still for 4.0 s so you can look at it and
 * photograph it:
 *
 *      pixels 0..7   RED
 *      pixels 8..14  BLUE
 *
 * This single still frame is the whole topology answer, because topology is a
 * SPATIAL question and this asks it spatially. Nothing needs to be timed or
 * counted - if the frame is on screen, the answer is on screen:
 *
 *   A) HALF the strip red, the other half blue -> HEALTHY CHAIN. Each LED is
 *      receiving its OWN slice of the bitstream. There is no wiring fault to
 *      find. (Which half is red also reports the physical order: if the BLUE
 *      half is the half the data enters from, the strip runs backwards
 *      relative to the data index - harmless, just mirrored.)
 *
 *   B) ALL FIFTEEN LEDs red, and the blue half NEVER appears no matter how
 *      long you watch -> PARALLEL / BUS. Every DIN is on one net, so every LED
 *      receives the SAME first 24 bits and pixel 0 (red) is all any of them can
 *      ever show. This is a LAYOUT/BOARD fault. The fix is to cut the common
 *      net and wire DOUT(n) -> DIN(n+1) - NOT to add a jumper.
 *
 *   C) FEWER than fifteen LEDs lit -> the LIT COUNT is how many LEDs the data
 *      actually reaches, and the position of the red/blue boundary is where
 *      the chain breaks. Fly a wire from the last LED that responds to the
 *      next one's DIN.
 *
 *   A photograph of this frame is the best evidence you can send: it shows how
 *   many LEDs are lit, where the colour boundary falls, and whether the two
 *   halves are distinguishable at all.
 *
 * PHASE 2 - THE WALK, one data index at a time, each in its own colour:
 *
 *   index 0  - RED,    held 3.0 s   (long, so you cannot miss the anchor)
 *   index 1  - orange, held 0.9 s
 *   ...        (a 15-step hue ramp; each data index gets its own colour)
 *   index 14 - pink,   held 0.9 s
 *   then 1.75 s with the whole strip dark, and the cycle repeats
 *
 *   In a healthy chain exactly ONE physical LED is lit at a time and it walks
 *   along the strip, so the lit position names the data index of that physical
 *   LED. In a parallel strip every step lights all fifteen in that step's
 *   colour instead.
 *
 * THERE IS NO WAY OUT BUT THE POWER SWITCH, ON PURPOSE
 * ----------------------------------------------------
 * Three earlier revisions handed the strip back to the rainbow effect on a key
 * press. Three times the strip was showing rainbow before a single LED had
 * walked:
 *
 *   1. v15 exited on ANY key event with no guard at all. The boot phantom
 *      press - a floating matrix input, or a gated rail still coming up -
 *      landed inside the thread's own 400 ms settle sleep, so the run flag was
 *      already false when the loop was reached and the loop body never
 *      executed once.
 *   2. v16 added a 3 s grace window that DROPS every key event, expecting the
 *      phantom press to become unable to satisfy a hold. It cannot START a
 *      hold during the window - but the window only covers the first 3.4 s. A
 *      phantom press arriving AFTER it, on a pin that never releases, latches
 *      key_down and ends the run 600 ms later.
 *   3. v17 removed input entirely, and it WORKED: the split frame appeared,
 *      with a clean boundary and two different colours on the two sides.
 *      Outcome A - the chain is healthy, nothing to rewire. That same frame
 *      also proved the bit encoding correct (values arrived at the requested
 *      magnitude) and exposed the strip's real channel order as GRB rather than
 *      the RGB the vendor datasheet states, because the half asked for red lit
 *      green. Fixed in the overlay's color-mapping; this file was NOT touched
 *      for it, and a channel swap must never be papered over in a renderer.
 *
 * So this revision takes no key input at all: no listener, no subscription, no
 * gesture, nothing on the input path can reach it. To leave the walk, power the
 * board off; the probe restarts on the next boot, which is the point. Reflash
 * hw-bringup to get normal operation back.
 *
 * The other half of that guarantee lives in effects.c: under CHAINPROBE the
 * effects engine is inert and its render thread is NEVER CREATED, so this file
 * is provably the only writer on the strip. If a smooth gradient rainbow ever
 * appears while this build is flashed, the image running is not this build.
 *
 * Note on colours: values are capped at 200/255 so that outcome B (all fifteen
 * lit at once) stays well inside the supply budget: even all fifteen white is
 * ~225 mA on the ST-1209RGB (5 mA/channel class), within the MT3608 5 V rail,
 * versus hundreds of mA with WS2812B 5050.
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_ZMK_RGB_PLAYER_CHAINPROBE)

#include <zmk_rgbeffect/led_pixel.h>
#include <zmk_rgbeffect/effects.h>

/* Render thread runs at 12; this inspector is equally unimportant. */
#define CP_THREAD_PRIORITY  12

/* 1536, not 1024: this thread drives the same synchronous ~1.2 ms SPI burst
 * that the render thread's 1536-byte stack was sized for. A stack fault here
 * would reboot the board, and on a board with no console that is
 * indistinguishable from "nothing happened" - a diagnostic must not be able to
 * fail in a way that looks like silence. */
#define CP_STACK_SIZE       1536

#define CP_START_DELAY_MS   300     /* let the led_strip device settle    */

/* Phase 1: the split frame. Colour B is deliberately the one that does NOT
 * appear on a parallel strip, so "did the blue half ever show up?" is a
 * question with a binary answer. */
#define CP_SPLIT_AT         8       /* pixels 0..7 get colour A           */
#define CP_SPLIT_A_R      200
#define CP_SPLIT_A_G        0
#define CP_SPLIT_A_B        0
#define CP_SPLIT_B_R        0
#define CP_SPLIT_B_G        0
#define CP_SPLIT_B_B      200
#define CP_SPLIT_HOLD_MS  4000

/* Phase 2: the walk. */
#define CP_HEAD_HOLD_MS     3000    /* index 0: long, it is the key step  */
#define CP_STEP_HOLD_MS     900
#define CP_GAP_MS           250
#define CP_CYCLE_PAUSE_MS   1500

K_THREAD_STACK_DEFINE(cp_stack, CP_STACK_SIZE);
static struct k_thread cp_thread;

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

/* Phase 1: one static frame, two colours, a hard boundary at CP_SPLIT_AT.
 * Nothing about this depends on timing, so it survives being looked at late,
 * being photographed, or being described over a chat window. */
static void cp_show_split(void) {
    led_pixel_clear();
    for (int i = 0; i < LED_PIXEL_COUNT; i++) {
        if (i < CP_SPLIT_AT) {
            led_pixel_set((uint8_t)i, CP_SPLIT_A_R, CP_SPLIT_A_G, CP_SPLIT_A_B);
        } else {
            led_pixel_set((uint8_t)i, CP_SPLIT_B_R, CP_SPLIT_B_G, CP_SPLIT_B_B);
        }
    }
    led_pixel_update();
}

/* Phase 2: exactly one data index lit, everything else black. */
static void cp_show_index(int idx) {
    led_pixel_clear();
    led_pixel_set((uint8_t)idx, cp_pal[idx][0], cp_pal[idx][1], cp_pal[idx][2]);
    led_pixel_update();
}

static void cp_thread_fn(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    /* Let the led_strip device settle, then take the strip over for good. */
    k_sleep(K_MSEC(CP_START_DELAY_MS));

    /* Phase 1, ONCE: the still frame that answers the topology question. */
    cp_show_split();
    k_sleep(K_MSEC(CP_SPLIT_HOLD_MS));

    /* Phase 2, forever: the walk. No exit condition exists - see the file
     * header for why every one that was ever added got here. */
    for (;;) {
        for (int i = 0; i < LED_PIXEL_COUNT; i++) {
            cp_show_index(i);
            k_sleep(K_MSEC(i == 0 ? CP_HEAD_HOLD_MS : CP_STEP_HOLD_MS));
            cp_all_off();
            k_sleep(K_MSEC(CP_GAP_MS));
        }
        k_sleep(K_MSEC(CP_CYCLE_PAUSE_MS));   /* all fifteen stay dark here */
    }
}

void chainprobe_init(void) {
    k_thread_create(&cp_thread, cp_stack,
                    K_THREAD_STACK_SIZEOF(cp_stack),
                    cp_thread_fn, NULL, NULL, NULL,
                    CP_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&cp_thread, "chainprobe");
}

#else

void chainprobe_init(void) { }

#endif /* CONFIG_ZMK_RGB_PLAYER_CHAINPROBE */
