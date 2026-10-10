/*
 * user_keymap.h - runtime, NVS-persisted manual key mapping
 *
 * The 15 physical keys (positions 0..14) each emit, when pressed in MANUAL
 * typing mode, the HID usage ID stored in user_keymap_table[pos]. The table is
 * loaded from NVS at boot (via the Zephyr settings subsystem) and can be
 * rewritten at runtime over BLE (command 0x40), so end users can remap the
 * board to any keys they like without reflashing.
 *
 * The default table equals the 光遇 (Sky) note layout (Y U I O P / H J K L ; /
 * N M , . /) so the board works out-of-the-box exactly as before. The AUTO
 * playback path (player.c KEY_SCANCODE) is intentionally NOT touched - that one
 * stays pinned to the game's notes; only the manual path is user-customizable.
 */
#pragma once
#include <stdint.h>

#define USER_KEYMAP_SLOTS 15

/* Per-position HID usage ID (Keyboard/Keypad page 0x07) emitted on manual press.
 * Index = physical key position 0..14. */
extern uint8_t user_keymap_table[USER_KEYMAP_SLOTS];

/* Persist the current table to NVS. Call after any change. */
void user_keymap_save(void);

/* Restore the factory 光遇 default and persist it. */
void user_keymap_reset_defaults(void);

/* Accessors (bounds-checked). */
uint8_t user_keymap_get_slot(uint8_t pos);
void    user_keymap_set_slot(uint8_t pos, uint8_t usage);
