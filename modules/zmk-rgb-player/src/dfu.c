/*
 * dfu.c - request UF2 bootloader entry (ZMK+nRF52840 default)
 *
 * ZMK by default uses the Adafruit UF2 bootloader on nice!nano.
 * The entry magic value is 0x57 written to GPREGRET before reset.
 * (cf. https://github.com/adafruit/Adafruit_nRF52_Bootloader)
 *
 * WHY NVIC_SystemReset() AND NOT sys_reboot():
 * ---------------------------------------------------------------
 * On nRF52, sys_reboot() is NOT a plain NVIC reset. When
 * CONFIG_NRF_STORE_REBOOT_TYPE_GPREGRET is enabled (it is by default on
 * nRF52 when REBOOT=y and RETENTION_BOOT_MODE is off), Zephyr overrides
 * the weak ARM implementation with:
 *
 *     void sys_arch_reboot(int type)
 *     {
 *         nrf_power_gpregret_set(NRF_POWER, (uint8_t)type);
 *         NVIC_SystemReset();
 *     }
 *
 *   -- zephyr/soc/arm/nordic_nrf/nrf52/soc.c
 *
 * i.e. it WRITES ITS OWN ARGUMENT OVER GPREGRET. SYS_REBOOT_WARM == 0,
 * so the sequence
 *
 *     NRF_POWER->GPREGRET = 0x57;
 *     sys_reboot(SYS_REBOOT_WARM);
 *
 * leaves GPREGRET == 0x00, not 0x57. The bootloader then evaluates
 *     uf2_dfu = (gpregret == DFU_MAGIC_UF2_RESET);   // 0x57
 * as false, dfu_start is false, and it resets straight back into the
 * application - USB disconnects and reconnects, but no NICENANO drive
 * ever appears. That is the exact symptom this function had.
 *
 * NVIC_SystemReset() goes to SCB->AIRCR directly, never calling
 * sys_arch_reboot(), so it leaves GPREGRET alone.
 *
 * DO NOT "fix" this by switching to ZMK's stock &bootloader behavior: it
 * has the same defect (app/src/behaviors/behavior_reset.c carries a
 * "TODO: Correct magic code for going into DFU?" and never writes
 * GPREGRET at all).
 *
 * Double-tap RESET stays the recovery path - it uses the bootloader's own
 * double-reset detection and never touches GPREGRET.
 */
#include <zephyr/kernel.h>
#include <hal/nrf_power.h>
#include <cmsis_core.h>
#include <zmk_rgbeffect/dfu.h>

void dfu_request(void) {
    /* Give the log a chance to flush. */
    k_msleep(50);
    NRF_POWER->GPREGRET = 0x57; /* DFU_MAGIC_UF2_RESET */
    NVIC_SystemReset();         /* NOT sys_reboot(): see the comment above */
}