/*
 * led_pixel.h - per-pixel WS2812B driver wrapper for the 15-key ZMK keyboard
 *
 * Public API used by effects.c / player.c / behaviors:
 *   led_pixel_init()                - bind to the chosen &led_strip device
 *   led_pixel_set(idx, r, g, b)     - stage a pixel color
 *   led_pixel_clear()               - clear all 15 staged colors to (0,0,0)
 *   led_pixel_update()              - push staged colors to the strip
 *   led_pixel_set_and_update(...)   - convenience: stage + push
 *   led_pixel_count()               - return 15
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define LED_PIXEL_COUNT 15

int  led_pixel_init(void);
void led_pixel_set(uint8_t idx, uint8_t r, uint8_t g, uint8_t b);
void led_pixel_clear(void);
void led_pixel_update(void);
void led_pixel_set_and_update(uint8_t idx, uint8_t r, uint8_t g, uint8_t b);
uint8_t led_pixel_count(void);
