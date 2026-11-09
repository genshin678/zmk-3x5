/*
 * led_pixel.h - per-pixel WS2812B driver wrapper for the 15-key ZMK keyboard
 *
 * Public API used by effects.c / player.c / behaviors:
 *   led_pixel_init()                - bind to the chosen &led_strip device
 *   led_pixel_set(idx, r, g, b)     - stage a pixel color
 *   led_pixel_clear()               - clear all LED_PIXEL_COUNT colors to black
 *   led_pixel_update()              - push staged colors to the strip
 *   led_pixel_set_and_update(...)   - convenience: stage + push
 *   led_pixel_count()               - return LED_PIXEL_COUNT
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <zephyr/drivers/led_strip.h>   /* struct led_rgb, color order */

/* Full 15-pixel board (see zmk_3x5_bt.overlay chain-length). This MUST stay
 * equal to chain-length - it is NOT derived from devicetree, so the two have
 * to be changed together. */
#ifndef LED_PIXEL_COUNT
#define LED_PIXEL_COUNT 15
#endif

int  led_pixel_init(void);
void led_pixel_set(uint8_t idx, uint8_t r, uint8_t g, uint8_t b);
void led_pixel_clear(void);
void led_pixel_update(void);
void led_pixel_set_and_update(uint8_t idx, uint8_t r, uint8_t g, uint8_t b);
uint8_t led_pixel_count(void);

/* One-pixel temporary override, applied inside led_pixel_update() on top of
 * whatever the current renderer staged.
 *
 * Used by pure_mode.c for the "K blinks twice" feedback. It has to work no
 * matter who owns the strip at that instant - the ambient effect thread, Mode C
 * cue frames, or the score player - and it must not disturb the renderer's own
 * buffer, because the blink ends by simply dropping the override and letting
 * the next frame through. That rules out both "stop the tick and paint" (kills
 * the cue mid-song) and "write the pixel into frame" (the renderer would have
 * to repaint it before the real colour came back). */
void led_pixel_set_override(uint8_t idx, uint8_t r, uint8_t g, uint8_t b);
void led_pixel_clear_override(void);
bool led_pixel_override_active(void);
