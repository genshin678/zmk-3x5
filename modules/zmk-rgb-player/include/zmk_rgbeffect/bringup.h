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

/* L3 diagnostic: the LED thread polls the live HID keyboard report every 20 ms
 * and latches a sticky flag if a non-zero keycode is ever seen. That is what
 * separates "the firmware put a real keycode in the report" (fault is then on
 * the host / BLE-link side) from "press and release merged into an all-zero
 * report" (a timing fault) - two states that look identical from the host.
 * Compiles to a no-op unless CONFIG_ZMK_RGB_PLAYER_BRINGUP=y.
 *
 * HISTORY: this used to be a one-shot "+50 ms sample" armed from &kp_we. That
 * design produced FALSE NEGATIVES - any press/release pair that both landed
 * before the sample reported "empty" and sent the investigation chasing a
 * timing theory that was never there. Never use a single delayed sample to
 * make a causal claim; poll and latch. */
void bringup_l3_signal(void);
