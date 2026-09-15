/*
 * bringup.h - key-independent hardware self test (diagnostic builds only)
 *
 * Compiled in only when CONFIG_ZMK_RGB_PLAYER_BRINGUP=y.
 * See src/bringup.c for what the self test does and why it exists.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Arm the self test. Call after led_pixel_init() / effects_init(). */
void bringup_init(void);

/* Report a logical key event (index 0..14) so the strip can display it. */
void bringup_key_event(int8_t key_index, bool pressed);
