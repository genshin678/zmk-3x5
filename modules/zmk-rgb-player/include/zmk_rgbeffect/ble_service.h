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
 *   Status          [READ+NOTIFY] - 8 bytes: state(1), mode(1), pos_ms(4), step(2)
 *   Events          [NOTIFY] - 4 bytes: event_code(1), key(1), step_lo(1), step_hi(1)
 *                            used by Mode C to report HIT/MISS/TIMEOUT/DONE
 *
 * NOTE: PLAYER_CTRL_* command opcodes are defined in player.h (single source of
 * truth) to avoid macro redefinition. Mode C command opcodes are defined below.
 */
#pragma once
#include <stdint.h>

/* Custom 128-bit service UUID: 9e3c1b0a-7a4b-4f0e-8d1d-7a6f5c3b2a10
 * Bytes are in textual-UUID (big-endian) order — NOT reversed.
 * (An earlier build accidentally used the byte-reversed form
 *  102a3b5c-6f7a-1d8d-0e4f-4b7a0a1b3c9e; the PWA tolerates both.) */
#define ZMK_PLAYER_SERVICE_UUID BT_UUID_DECLARE_128( \
    0x9e, 0x3c, 0x1b, 0x0a, 0x7a, 0x4b, 0x4f, 0x0e, \
    0x8d, 0x1d, 0x7a, 0x6f, 0x5c, 0x3b, 0x2a, 0x10)

/* 16-bit characteristic UUIDs (unique per characteristic). */
#define ZMK_PLAYER_CHRC_SCORE      0xBE01  /* WRITE  */
#define ZMK_PLAYER_CHRC_CONTROL    0xBE02  /* WRITE  */
#define ZMK_PLAYER_CHRC_KEYPRESS   0xBE03  /* WRITE  */
#define ZMK_PLAYER_CHRC_STATUS     0xBE04  /* READ+NOTIFY */
#define ZMK_PLAYER_CHRC_EVENTS     0xBE05  /* NOTIFY (Mode C events) */

/* Mode C (Assisted Play-Along) control commands (in the CONTROL characteristic). */
#define MODE_C_START  0x30  /* + u16 note_count */
#define MODE_C_PUSH   0x31  /* + u16 delta_ms, u8 key(1..15), u8 duration_ms */
#define MODE_C_TICK   0x32  /* + u32 app_ms (reference clock, informational) */
#define MODE_C_STOP   0x35  /* (no params) */

int ble_service_init(void);
