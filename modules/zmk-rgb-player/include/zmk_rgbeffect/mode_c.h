/*
 * mode_c.h - Assisted Play-Along ("引导弹奏") mode
 *
 * The phone App pushes the score step-by-step, then sends START. The keyboard
 * shows the current step's key in BLUE and the next step's key in RED.
 * The user presses the physical key; on the correct key we flash GREEN and
 * advance; on a wrong key we flash ALL RED and report MISS; on timeout we
 * flash the current key RED and report TIMEOUT, then skip to the next step.
 *
 * BLE control commands (see ble_service.h):
 *   0x30 MODE_C_START  + u16 note_count
 *   0x31 MODE_C_PUSH   + u16 delta_ms, u8 key(1..15), u8 duration_ms
 *   0x32 MODE_C_TICK   + u32 app_ms  (reference clock, informational)
 *   0x35 MODE_C_STOP
 *
 * Events reported back to the App via the EVENTS characteristic (BE05):
 *   0x01 HIT     (key = correct key pressed)
 *   0x02 MISS    (key = wrong key pressed)
 *   0x03 TIMEOUT (key = 0)
 *   0x04 DONE    (key = 0, all steps complete)
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define MODE_C_MAX_STEPS 2048

/* Event opcodes sent on the BLE EVENTS characteristic. */
#define MODE_C_EVT_HIT     0x01
#define MODE_C_EVT_MISS    0x02
#define MODE_C_EVT_TIMEOUT 0x03
#define MODE_C_EVT_DONE    0x04

int  mode_c_init(void);
void mode_c_start(uint16_t note_count);
void mode_c_push(uint16_t delta_ms, uint8_t key, uint8_t duration_ms);
void mode_c_tick(uint32_t app_ms);
void mode_c_stop(void);

/* Called from the ZMK position listener to report a physical key press. */
void mode_c_on_position(uint32_t position, bool pressed);

bool     mode_c_is_active(void);
uint16_t mode_c_current_step(void);

/* Implemented in ble_service.c; emits a BE05 notification. */
void mode_c_notify_event(uint8_t event, uint8_t key, uint16_t step);
