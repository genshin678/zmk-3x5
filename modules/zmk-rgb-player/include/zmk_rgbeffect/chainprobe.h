/*
 * chainprobe.h - WS2812 daisy-chain inspector (TEMPORARY diagnostic)
 *
 * See src/chainprobe.c for what the pattern means and how to read it.
 * Compile-time gated by CONFIG_ZMK_RGB_PLAYER_CHAINPROBE; the symbol always
 * exists so callers need no #ifdef.
 */
#pragma once

void chainprobe_init(void);
