/*
 * cdc.h - USB CDC command channel (PC wired transport). See src/cdc.c.
 *
 * The channel exists so a score can be pushed into the board over a USB cable,
 * which is the only way to play wired from a PC: the keyboard side of a plugged
 * in board is an ordinary HID keyboard on the host, and nothing else on the
 * cable carries control traffic.
 *
 * The prototype below is deliberately not conditional on a Kconfig symbol.
 * ble_service.c's mode_c_notify_event() reports every mode_c event to the phone
 * over BE05; the same events go down the cable so a wired session is not blind.
 * A no-op definitions when the channel is compiled out keeps that call site
 * unconditional, and therefore keeps the two builds from drifting apart.
 */
#pragma once
#include <stdint.h>

/* Called for every MODE_C_EVT_* ; emits an "EVT:<name>,<key>,<step>" line. */
void cdc_emit_event(uint8_t event, uint8_t key, uint16_t step);

/* Installs the UART callback. Safe to call unconditionally. */
int  pc_uart_init(void);
