/*
 * effects.h - effect IDs and public API
 */
#pragma once
#include <stdint.h>

typedef enum {
    RGB_EFFECT_OFF          = 0,
    RGB_EFFECT_SOLID        = 1,
    RGB_EFFECT_BREATHING    = 2,
    RGB_EFFECT_RAINBOW      = 3,
    RGB_EFFECT_SINGLE_KEY   = 4,
    RGB_EFFECT_TWINKLE      = 5,
    RGB_EFFECT_RIPPLE       = 6,
    RGB_EFFECT_COUNT        = 7
} rgb_effect_t;

#define LED_PIXEL_COUNT 15

void effects_init(void);
void effects_tick_start(void);
void effects_tick_stop(void);
void effects_set_active(rgb_effect_t e);
rgb_effect_t effects_get_active(void);
void effects_next(int direction);
void effects_set_active_key(int8_t key_index);

/* Called by player / ble_service when a note fires. */
void effects_on_key_down(int8_t key_index);
void effects_on_key_up(int8_t key_index);

/* Player takeover (stops all effects while score plays). */
void effects_player_enter(void);
void effects_player_exit(void);

/* LED pixel primitive (called by player for per-key color). */
void led_pixel_set_and_update(uint8_t idx, uint8_t r, uint8_t g, uint8_t b);
void led_pixel_clear(void);
void led_pixel_update(void);