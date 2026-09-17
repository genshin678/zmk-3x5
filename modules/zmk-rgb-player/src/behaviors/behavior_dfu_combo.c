/*
 * behavior_dfu_combo.c - P+N held 3s -> reboot into UF2 bootloader
 *
 * Bound to the P+N combo (key-positions <4 10>) in the keymap.
 *
 * ZMK fires a combo behavior once with a *virtual* event.position
 * (ZMK_VIRTUAL_KEY_POSITION_COMBO), so the previous per-physical-position
 * bitmask test (`held_mask != 0x3`) could never succeed — see
 * behavior_bri_dual_hold.c for the full explanation. We track held state with
 * a bool instead.
 *
 * On combo press  : schedule work for 3000 ms.
 * On combo release: cancel (so a normal P+N tap does nothing).
 * On fire         : write GPREGRET = 0x57 and reset into the bootloader.
 *
 * WHY NVIC_SystemReset() AND NOT sys_reboot() — THIS WAS A REAL BUG:
 * ---------------------------------------------------------------
 * The entry order matters, and it used to be wrong:
 *
 *     NRF_POWER->GPREGRET = 0x57;      // magic written...
 *     sys_reboot(SYS_REBOOT_WARM);     // ...then clobbered
 *
 * On nRF52, sys_reboot() is not a plain NVIC reset. With
 * CONFIG_NRF_STORE_REBOOT_TYPE_GPREGRET enabled (the default on nRF52
 * whenever REBOOT=y and RETENTION_BOOT_MODE is off), Zephyr overrides the
 * weak ARM implementation with
 *
 *     void sys_arch_reboot(int type)
 *     {
 *         nrf_power_gpregret_set(NRF_POWER, (uint8_t)type);
 *         NVIC_SystemReset();
 *     }
 *
 *   -- zephyr/soc/arm/nordic_nrf/nrf52/soc.c
 *
 * i.e. it writes its own argument over GPREGRET. SYS_REBOOT_WARM is 0, so
 * GPREGRET ended up 0x00 rather than 0x57, the bootloader's
 *
 *     uf2_dfu = (gpregret == DFU_MAGIC_UF2_RESET);   // 0x57
 *
 * evaluated false, and it reset straight back into the application. The
 * observable symptom was USB disconnecting and reconnecting with no
 * NICENANO drive - a "P+N does nothing" that looked like a gesture problem
 * but was never one. It had never worked, not once.
 *
 * NVIC_SystemReset() goes to SCB->AIRCR directly and never calls
 * sys_arch_reboot(), so GPREGRET survives intact.
 *
 * DO NOT replace this with ZMK's stock &bootloader behavior: it carries the
 * same defect (app/src/behaviors/behavior_reset.c has a "TODO: Correct magic
 * code for going into DFU?" and does not write GPREGRET at all).
 *
 * The double-tap-RESET path remains unaffected as the recovery route: it
 * rides the bootloader's own double-reset detection (DFU_DBL_RESET_MAGIC in
 * RAM, gated on RESETREAS.RESETPIN) and never consults GPREGRET.
 */
#define DT_DRV_COMPAT zmk_behavior_dfu_combo

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_power.h>
#include <cmsis_core.h>
#include <drivers/behavior.h>

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

static bool combo_held;
static struct k_work_delayable dfu_hold_work;

static void fire_dfu(struct k_work *w) {
    ARG_UNUSED(w);
    if (!combo_held) {
        return;
    }
    NRF_POWER->GPREGRET = 0x57; /* DFU_MAGIC_UF2_RESET */
    NVIC_SystemReset();         /* NOT sys_reboot(): see the header comment */
}

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    combo_held = true;
    k_work_init_delayable(&dfu_hold_work, fire_dfu);
    k_work_schedule(&dfu_hold_work, K_MSEC(3000));
    return 0;
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    combo_held = false;
    k_work_cancel_delayable(&dfu_hold_work);
    return 0;
}

static const struct behavior_driver_api api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};

#define DFU_COMBO_INST(n)                                                                          \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &api);

DT_INST_FOREACH_STATUS_OKAY(DFU_COMBO_INST)
