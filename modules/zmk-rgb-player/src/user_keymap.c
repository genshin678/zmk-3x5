/*
 * user_keymap.c - runtime, NVS-persisted manual key mapping
 *
 * See user_keymap.h. The table lives in RAM, is initialised to the 光遇 default,
 * and is overwritten from NVS at boot if a stored value exists. Writes go through
 * the Zephyr settings subsystem (ZMK's NVS backend), so no manual flash handling
 * is needed and the data survives reboots and firmware updates that keep the
 * settings partition.
 */
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <string.h>
#include <zmk_rgbeffect/user_keymap.h>

/* Default = 光遇 note layout (pos 0..14 = Y U I O P / H J K L ; / N M , . /).
 * HID usage IDs, Keyboard/Keypad page 0x07. Verified against a real build dump. */
static const uint8_t user_keymap_default[USER_KEYMAP_SLOTS] = {
    0x1C, 0x18, 0x0C, 0x12, 0x13,  /* Y U I O P */
    0x0B, 0x0D, 0x0E, 0x0F, 0x33,  /* H J K L ; */
    0x11, 0x10, 0x36, 0x37, 0x38,  /* N M , . / */
};

uint8_t user_keymap_table[USER_KEYMAP_SLOTS] = {
    0x1C, 0x18, 0x0C, 0x12, 0x13,
    0x0B, 0x0D, 0x0E, 0x0F, 0x33,
    0x11, 0x10, 0x36, 0x37, 0x38,
};

/* settings set callback: reads the stored "table" blob into RAM. */
static int user_km_set(const char *name, size_t len,
                       settings_read_cb read_cb, void *cb_arg) {
    if (strcmp(name, "table") == 0 && len == USER_KEYMAP_SLOTS) {
        uint8_t buf[USER_KEYMAP_SLOTS];
        ssize_t got = read_cb(cb_arg, buf, len);
        if (got == (ssize_t)len) {
            memcpy(user_keymap_table, buf, len);
        }
    }
    return 0;
}

/* Registers a handler for the "user_km" subtree. ZMK's settings init
 * (POST_KERNEL) registers this and calls settings_load(), so the table is
 * populated before any keypress can occur (our module init is APPLICATION). */
SETTINGS_STATIC_HANDLER_DEFINE(user_km, "user_km", NULL, user_km_set, NULL, NULL);

void user_keymap_save(void) {
    settings_save_one("user_km/table", user_keymap_table, USER_KEYMAP_SLOTS);
}

void user_keymap_reset_defaults(void) {
    memcpy(user_keymap_table, user_keymap_default, USER_KEYMAP_SLOTS);
    user_keymap_save();
}

uint8_t user_keymap_get_slot(uint8_t pos) {
    return pos < USER_KEYMAP_SLOTS ? user_keymap_table[pos] : 0;
}

void user_keymap_set_slot(uint8_t pos, uint8_t usage) {
    if (pos < USER_KEYMAP_SLOTS) {
        user_keymap_table[pos] = usage;
    }
}
