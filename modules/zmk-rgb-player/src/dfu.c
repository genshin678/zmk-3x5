/*
 * dfu.c - request UF2 bootloader entry (ZMK+nRF52840 default)
 *
 * ZMK by default uses the Adafruit UF2 bootloader on nice!nano.
 * The entry magic value is 0x57 written to GPREGRET before reset.
 * (cf. https://github.com/adafruit/Adafruit_nRF52_Bootloader)
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>
#include <zmk_rgbeffect/dfu.h>

void dfu_request(void) {
    /* Give the log a chance to flush. */
    k_msleep(50);
    NRF_POWER->GPREGRET = 0x57;
    NVIC_SystemReset();
}
