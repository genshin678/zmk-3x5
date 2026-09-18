/*
 * ble_service.h - ZMK Player custom BLE GATT service
 *
 * Service UUID (custom 128-bit):
 *   9e3c1b0a-7a4b-4f0e-8d1d-7a6f5c3b2a10
 *
 * Characteristics:
 *   Score Upload    [WRITE]  - accepts the binary score blob in chunks
 *   Playback Ctrl   [WRITE]  - PLAY=0x01 / PAUSE=0x02 / STOP=0x03 / MODE=0x10 / CLEAR=0x20
 *   Live Keypress   [WRITE]  - single byte 1..15, fires immediate LED + (mode B) HID
 *   Status          [READ+NOTIFY] - 9 bytes: state(1), mode(1), pos_ms(4), step(2),
 *                            fw_flags(1)
 *   Events          [NOTIFY] - 4 bytes: event_code(1), key(1), step_lo(1), step_hi(1)
 *                            used by Mode C to report HIT/MISS/DONE/STEP
 *
 * NOTE: PLAYER_CTRL_* command opcodes are defined in player.h (single source of
 * truth) to avoid macro redefinition. Mode C command opcodes are defined below.
 */
#pragma once
#include <stdint.h>

/* Custom 128-bit service UUID: 9e3c1b0a-7a4b-4f0e-8d1d-7a6f5c3b2a10
 *
 * Byte order matters and it is NOT the textual order.
 *
 * `BT_UUID_DECLARE_128()` takes the *on-air* byte array, and Bluetooth transmits
 * 128-bit UUIDs least-significant-byte first. Writing the bytes out in textual
 * order (`0x9e,0x3c,0x1b,0x0a,...`) therefore puts them on the wire reversed and
 * every peer decodes the service as
 * `102a3b5c-6f7a-1d8d-0e4f-4b7a0a1b3c9e` instead of the intended value - which
 * is what this file did for a long time, while a comment here blamed "an earlier
 * build" for a reversal the declaration itself caused.
 *
 * The array below is therefore the REVERSE of the textual octets, and it is written
 * out by hand rather than through `BT_UUID_128_ENCODE()` on purpose: a macro hides the
 * one detail this file has already got wrong once, and this form can be checked by
 * eye (and by scanning the built UF2) against the rule above, which is measured -
 * an old build spelled the octets in textual order and every peer decoded exactly
 * the reverse of the intended string.
 * (The Android app matches by prefix and accepts either encoding, so a board
 * still running the old form connects too.) */
#define ZMK_PLAYER_SERVICE_UUID BT_UUID_DECLARE_128( \
    0x10, 0x2a, 0x3b, 0x5c, 0x6f, 0x7a, 0x1d, 0x8d, \
    0x0e, 0x4f, 0x4b, 0x7a, 0x0a, 0x1b, 0x3c, 0x9e)

/* 16-bit characteristic UUIDs (unique per characteristic). */
#define ZMK_PLAYER_CHRC_SCORE      BT_UUID_DECLARE_16(0xBE01)  /* WRITE  */
#define ZMK_PLAYER_CHRC_CONTROL    BT_UUID_DECLARE_16(0xBE02)  /* WRITE  */
#define ZMK_PLAYER_CHRC_KEYPRESS   BT_UUID_DECLARE_16(0xBE03)  /* WRITE  */
#define ZMK_PLAYER_CHRC_STATUS     BT_UUID_DECLARE_16(0xBE04)  /* READ+NOTIFY */
#define ZMK_PLAYER_CHRC_EVENTS     BT_UUID_DECLARE_16(0xBE05)  /* NOTIFY (Mode C events) */

/* Mode C (Assisted Play-Along) control commands (in the CONTROL characteristic).
 *
 * A step is a CHORD: MODE_C_PUSH carries a 15-bit key mask, not a single key, so
 * one step maps to one cell of the printed sheet. An App that only knows the older
 * 5-byte PUSH layout must not send it to this build - it would be read as a mask
 * with the wrong bytes. BE04 status byte 8 bit1 advertises the change. */
#define MODE_C_START  0x30  /* + u16 note_count */
#define MODE_C_PUSH   0x31  /* + u16 delta_ms, u16 key_mask, u8 duration_ms */
#define MODE_C_TICK   0x32  /* + u32 app_ms (reference clock, informational) */
#define MODE_C_STOP   0x35  /* (no params) aborts playback, clears the step table */
#define MODE_C_PACE   0x36  /* + u8 pace: 0 = manual (press to advance), 1 = auto */

int ble_service_init(void);
