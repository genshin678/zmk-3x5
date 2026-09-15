/*
 * btdiag.h - BLE link-latency probe (TEMPORARY)
 *
 * Measures the connection parameters the HOST actually granted, because ZMK
 * never asks for them: app/src/ble.c registers only the passive
 * le_param_updated callback and contains no bt_conn_le_param_update() call.
 * The read-out is on the on-board blue LED.
 *
 * Gated by CONFIG_ZMK_RGB_PLAYER_BTDIAG. When that is off this compiles to an
 * empty function and the probe costs nothing. See USBRGB-verify.md.
 */
#pragma once

void btdiag_init(void);
