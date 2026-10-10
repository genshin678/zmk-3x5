/*
 * player.h - score playback API
 */
#pragma once
#include <stdint.h>

typedef enum { PLAYER_STOPPED, PLAYER_PAUSED, PLAYER_PLAYING } player_state_t;
typedef enum { PLAYER_MODE_A,  PLAYER_MODE_B }                  player_mode_t;

#define PLAYER_MAX_NOTES   2000
#define PLAYER_BUF_BYTES   (16 * 1024)

/* Number of physical keys on the board. */
#define PLAYER_KEY_COUNT   15

/* Raw HID usage IDs (Keyboard/Keypad page 0x07) for keyIndex 1..15: element k is the
 * usage ID of key k+1, which is exactly what zmk_hid_keyboard_press() takes.
 *
 * player.c owns the only copy, so anything else that synthesises a keystroke (mode_c's
 * auto pace) reads it from here instead of repeating the numbers - the two playback
 * paths can then never disagree. The authoritative table is the keymap in
 * config/zmk_3x5_bt.keymap; see the long note there before changing any value. */
const uint8_t *player_key_scancodes(void);

#define PLAYER_CTRL_PLAY       0x01
#define PLAYER_CTRL_PAUSE      0x02
#define PLAYER_CTRL_STOP       0x03
#define PLAYER_CTRL_MODE_A     0x10
#define PLAYER_CTRL_MODE_B     0x11
#define PLAYER_CTRL_CLEAR      0x20
#define PLAYER_CTRL_LOAD_DONE  0x21

int  player_init(void);
int  player_load(const uint8_t *buf, uint16_t len);
void player_clear(void);
int  player_play(void);
int  player_pause(void);
int  player_stop(void);
void player_set_mode(player_mode_t m);
player_mode_t player_get_mode(void);
player_state_t player_get_state(void);
uint32_t       player_get_position_ms(void);