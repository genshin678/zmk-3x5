/*
 * led_pixel.c - per-pixel WS2812B driver wrapper
 *
 * Uses Zephyr's led_strip API on the device chosen by zmk,led-strip in the
 * overlay.  Bypasses ZMK's &rgb_ug so per-pixel control is fully ours.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>
#include <zmk_rgbeffect/led_pixel.h>

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

static const struct device *strip;
static struct led_rgb frame[LED_PIXEL_COUNT];
static bool initialized;
static K_MUTEX_DEFINE(led_mutex);

int led_pixel_init(void) {
    strip = DEVICE_DT_GET(DT_CHOSEN(zmk_led_strip));
    if (!device_is_ready(strip)) {
        LOG_ERR("LED strip device not ready");
        return -ENODEV;
    }
    memset(frame, 0, sizeof(frame));
    initialized = true;
    LOG_INF("led_pixel ready: %d pixels", LED_PIXEL_COUNT);
    return 0;
}

void led_pixel_set(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    if (idx >= LED_PIXEL_COUNT) return;
    frame[idx].r = r;
    frame[idx].g = g;
    frame[idx].b = b;
}

void led_pixel_clear(void) {
    memset(frame, 0, sizeof(frame));
}

void led_pixel_update(void) {
    if (!initialized) return;
    k_mutex_lock(&led_mutex, K_FOREVER);
    int rc = led_strip_update_rgb(strip, frame, LED_PIXEL_COUNT);
    k_mutex_unlock(&led_mutex);
    if (rc < 0) {
        LOG_WRN("led_strip_update_rgb failed: %d", rc);
    }
}

void led_pixel_set_and_update(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    led_pixel_set(idx, r, g, b);
    led_pixel_update();
}

uint8_t led_pixel_count(void) {
    return LED_PIXEL_COUNT;
}
