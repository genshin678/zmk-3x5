/*
 * cdc.c - USB CDC debug console for loading scores without BLE
 *
 * Commands (line-based, LF terminated):
 *   PING                 -> responds PONG
 *   LOAD:<len>           -> expects <len> raw binary bytes after the newline
 *   PLAY | PAUSE | STOP
 *   MODE A | MODE B
 *   BRIGHTNESS <0-255>
 *   HUE <0-359>
 *   EFFECT <0-5>
 *   REBOOT DFU
 */
#include <stdlib.h>                    /* atoi() */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zmk_rgbeffect/player.h>
#include <zmk_rgbeffect/ble_service.h>
#include <zmk_rgbeffect/rgb_control.h>
#include <zmk_rgbeffect/dfu.h>

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

/* The entire console implementation exists only when a `zmk,console` chosen
 * node is declared - cdc_init() below already says so. Without an equivalent
 * guard around the parser, console_cb() (and the buffers it is the sole reader
 * of) are referenced by nothing, and -Wunused-function made every build print
 *
 *     zmk-rgb-player/src/cdc.c:32:13: warning: 'console_cb' defined but not used
 *
 * which is exactly the kind of noise that hides a real warning later. One guard
 * around the whole block removes it without changing behaviour, since none of
 * this code could run in that configuration anyway. */
#if DT_HAS_CHOSEN(zmk_console)

#define LINE_BUF_MAX  128
static char line_buf[LINE_BUF_MAX];
static size_t line_len;

static void process_line(const char *line);

static void console_cb(const struct device *dev, void *user_data) {
    uint8_t c;
    while (uart_poll_in(dev, &c) == 0) {
        if (c == '\n' || c == '\r') {
            if (line_len > 0) {
                line_buf[line_len] = 0;
                process_line(line_buf);
                line_len = 0;
            }
        } else if (line_len < LINE_BUF_MAX - 1) {
            line_buf[line_len++] = (char)c;
        }
    }
}

static void process_line(const char *line) {
    LOG_INF("cdc: %s", line);
    if (strcmp(line, "PING") == 0) {
        LOG_INF("PONG");
    } else if (strncmp(line, "MODE ", 5) == 0) {
        player_set_mode(line[5] == 'B' ? PLAYER_MODE_B : PLAYER_MODE_A);
    } else if (strncmp(line, "BRIGHTNESS ", 11) == 0) {
        rgb_control_set_brightness((uint8_t)atoi(line + 11));
    } else if (strncmp(line, "HUE ", 4) == 0) {
        rgb_control_set_hue((uint16_t)atoi(line + 4));
    } else if (strncmp(line, "EFFECT ", 7) == 0) {
        rgb_control_set_effect((rgb_effect_t)atoi(line + 7));
    } else if (strcmp(line, "PLAY") == 0) {
        player_play();
    } else if (strcmp(line, "PAUSE") == 0) {
        player_pause();
    } else if (strcmp(line, "STOP") == 0) {
        player_stop();
    } else if (strcmp(line, "REBOOT DFU") == 0) {
        dfu_request();
    } else {
        LOG_WRN("unknown cdc cmd: %s", line);
    }
}

#endif /* DT_HAS_CHOSEN(zmk_console) */

int cdc_init(void) {
#if DT_HAS_CHOSEN(zmk_console)
    const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zmk_console));
    if (!device_is_ready(dev)) {
        LOG_ERR("console device not ready");
        return -ENODEV;
    }
    uart_irq_callback_user_data_set(dev, console_cb, NULL);
    uart_irq_rx_enable(dev);
    LOG_INF("cdc console ready");
    return 0;
#else
    LOG_WRN("zmk,console chosen not defined; cdc console disabled");
    return -ENOSYS;
#endif
}
