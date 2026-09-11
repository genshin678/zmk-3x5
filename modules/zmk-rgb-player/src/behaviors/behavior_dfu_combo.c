/*
 * behavior_dfu_combo.c - P+N held 3s → reboot into UF2 bootloader
 *
 * Bound to P (pos 4) and N (pos 10) via held-combo.
 * On first press of either: schedule work for 3000ms.
 * On release of either before 3s: cancel.
 * On fire: write GPREGRET = 0x57 and reset.
 */
#include <zephyr/kernel.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>
#include <zmk/behavior.h>

static uint8_t held_mask;
static struct k_work_delayable work;

static void fire_dfu(struct k_work *work) {
    if (held_mask != 0b11) return;
    NRF_POWER->GPREGRET = 0x57;
    NVIC_SystemReset();
}

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event *event) {
    held_mask |= (1 << event->position);
    k_work_init_delayable(&work, fire_dfu);
    k_work_schedule(&work, K_MSEC(3000));
    return 0;
}
static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event *event) {
    held_mask &= ~(1 << event->position);
    k_work_cancel_delayable(&work);
    return 0;
}
static const struct zmk_behavior_driver_api api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};
ZMK_BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, void, dfu_combo, &api);