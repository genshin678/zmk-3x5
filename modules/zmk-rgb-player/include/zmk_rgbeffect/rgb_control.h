/*
 * rgb_control.h - global brightness / hue / effect / last-key state
 */
#pragma once
#include <stdint.h>
#include <zmk_rgbeffect/effects.h>

/* Brightness loop direction for Y+P simultaneous hold. */
typedef enum { BRI_IDLE, BRI_UP, BRI_DOWN } bri_loop_t;

int    rgb_control_init(void);

/* Brightness */
void   rgb_control_set_brightness(uint8_t v);
void   rgb_control_change_brightness(int delta);
uint8_t rgb_control_get_brightness(void);

/* Hue */
void   rgb_control_set_hue(uint16_t h);
void   rgb_control_change_hue(int delta);
uint16_t rgb_control_get_hue(void);

/* Effect */
rgb_effect_t rgb_control_get_effect(void);
void         rgb_control_set_effect(rgb_effect_t e);
void         rgb_control_next_effect(void);   /* cycle +1 */

/* Brightness loop (Y+P simultaneous hold) */
void   rgb_control_bri_loop_start(bri_loop_t dir);
void   rgb_control_bri_loop_stop(void);
bri_loop_t rgb_control_get_bri_loop(void);

/* Hue loop (H+; simultaneous hold) */
void   rgb_control_hue_loop_start(int8_t dir);   /* +1 or -1 */
void   rgb_control_hue_loop_stop(void);
int8_t  rgb_control_get_hue_loop_dir(void);

/* Last-pressed key index (for ripple wave origin) */
void   rgb_control_set_last_key(int8_t key);
int8_t  rgb_control_get_last_key(void);