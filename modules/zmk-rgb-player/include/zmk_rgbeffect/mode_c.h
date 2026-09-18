/*
 * mode_c.h - Assisted Play-Along ("引导弹奏" / "跟弹")
 *
 * The phone App pushes the score step-by-step, then sends START. The keyboard
 * shows the current step's keys in BLUE and the next step's keys in RED.
 *
 * A STEP IS A CHORD, NOT A NOTE. `key_mask` is a 15-bit set - bit (k-1) for key k
 * in 1..15 - and one step corresponds to ONE cell of the printed sheet, i.e. one
 * 小节, which is exactly how the source score groups notes. The step is satisfied
 * when every key in its mask has been pressed.
 *
 *   (An earlier build judged a single key per step and the App expanded a chord
 *    into several consecutive steps with an 80 ms gap. That made a chord feel like
 *    a fast arpeggio and put the lights out of step with the sheet. BE04 status
 *    byte 8 bit1 tells the App which of the two PUSH layouts this build accepts.)
 *
 * PACING - switchable at run time with 0x36 MODE_C_PACE, mid-session included:
 *
 *   MANUAL (default)  Strictly user-paced. Nothing advances on a timer. A step
 *                     clears only when all of its keys have been pressed - in any
 *                     order; simultaneity is not required, only completeness, so a
 *                     rolled chord counts. A wrong key flashes red and reports MISS
 *                     but does not move the cursor.
 *   AUTO              The keyboard PLAYS the score itself. Each step is shown for
 *                     its own `delta`, and every key of that step's mask is emitted to
 *                     the host as a real HID report, so the notes actually sound in the
 *                     game - not just on the strip. Physical presses are not judged.
 *
 * BLE control commands (see ble_service.h):
 *   0x30 MODE_C_START  + u16 note_count
 *   0x31 MODE_C_PUSH   + u16 delta_ms, u16 key_mask, u8 duration_ms
 *                      duration_ms is how long each emitted key is held down in AUTO
 *                      pace; it is clamped to 50..200 ms and 0 means the 80 ms default.
 *   0x32 MODE_C_TICK   + u32 app_ms  (reference clock, informational)
 *   0x35 MODE_C_STOP   (aborts playback AND clears the step table)
 *   0x36 MODE_C_PACE   + u8 pace (0 = manual, 1 = auto)
 *
 * Events reported back to the App via the EVENTS characteristic (BE05):
 *   0x01 HIT     key = lowest key of the satisfied chord, step = the step hit
 *   0x02 MISS    key = the wrong key that was pressed, step = the step you are on
 *   0x03 TIMEOUT key = 0 - never emitted: nothing times out. Opcode stays reserved
 *                so an App built against the old behaviour does not misread it.
 *   0x04 DONE    key = 0, step = step_count, all steps complete
 *   0x05 STEP    key = number of keys in the step just armed, step = its index.
 *                Emitted on every arm in BOTH paces, so a companion UI can follow
 *                the cursor without having to infer it from HITs - which is the
 *                only way AUTO mode can be tracked at all, since AUTO never HITs.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define MODE_C_MAX_STEPS 2048

/* The 15 keys of the strip as a mask: bit(k-1) for key k in 1..15. */
#define MODE_C_ALL_KEYS 0x7FFFu

/* Pacing values for MODE_C_PACE. */
#define MODE_C_PACE_MANUAL 0x00u
#define MODE_C_PACE_AUTO   0x01u

/* Maximum `delta` (ms) the firmware honours. Historically hard-coded to 1000 in
 * mode_c_on_position, which made any rest longer than 1s snap to 1s and ruined the
 * tempo of slow passages. `steps[].delta` is a uint16_t, so 65000 stays well inside
 * the 0..65535 wire range. Negotiated via BE04 status byte 8 bit0 (wide delta). */
#define MODE_C_DELTA_MAX_MS 65000

/* Floor on the post-HIT gap, in manual pace: short enough not to distort the tempo,
 * long enough for the 140 ms green flash to actually read as a confirmation. */
#define MODE_C_HIT_GAP_MIN_MS 140u

/* Floor on a step's own length in auto pace. A dense passage can legitimately ask
 * for a few tens of ms per cell; below that the strip just strobes. */
#define MODE_C_AUTO_MIN_MS 60u

/* Capability flags reported in BE04 status byte 8 (see ble_service.c).
 *   bit0: wide delta - `delta` is not clamped to 1s on a HIT
 *   bit1: chord mask  - MODE_C_PUSH carries a u16 key mask, not a u8 single key
 *   bit2: auto plays  - AUTO pace emits real HID keystrokes, so a score actually
 *                       sounds on the host. An App must NOT offer auto-play unless
 *                       this bit is set: on an older build AUTO only animates the
 *                       strip, which looks exactly like a broken feature. */
#define ZMK_RGB_PLAYER_FW_FLAGS 0x07u

/* Event opcodes sent on the BLE EVENTS characteristic. */
#define MODE_C_EVT_HIT     0x01
#define MODE_C_EVT_MISS    0x02
#define MODE_C_EVT_TIMEOUT 0x03   /* in the protocol, never emitted: see above */
#define MODE_C_EVT_DONE    0x04
#define MODE_C_EVT_STEP    0x05

int  mode_c_init(void);
void mode_c_start(uint16_t note_count);
void mode_c_push(uint16_t delta_ms, uint16_t key_mask, uint8_t duration_ms);
void mode_c_tick(uint32_t app_ms);
void mode_c_stop(void);

/* Pacing. Callable at any time, including mid-session and while idle: the value
 * persists across STOP/DONE, so the next session starts the way the user left it. */
void mode_c_set_pace(uint8_t pace);
uint8_t mode_c_get_pace(void);

/* Called from the ZMK position listener to report a physical key press. */
void mode_c_on_position(uint32_t position, bool pressed);

bool     mode_c_is_active(void);
uint16_t mode_c_current_step(void);
uint16_t mode_c_step_count(void);

/* Implemented in ble_service.c; emits a BE05 notification. */
void mode_c_notify_event(uint8_t event, uint8_t key, uint16_t step);
