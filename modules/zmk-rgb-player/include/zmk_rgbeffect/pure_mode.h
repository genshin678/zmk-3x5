/*
 * pure_mode.h - 纯净弹琴模式 (pure play mode)
 *
 * Why it exists
 * -------------
 * The board carries six function combos on the same fifteen keys a score needs
 * (brightness Y+P, hue H+;, effect Y+N, DFU P+N, BLE-bond wipe O+., endpoint
 * toggle I+K). Every one of them consumes its member keys and, on most of
 * them, changes the lights. While the user is actually playing, those are
 * liabilities: a chord that happens to match one of them silently drops two
 * notes, and a stray combination rewrites the strip mid-song.
 *
 * Pure mode removes the whole category in one gesture instead of asking the
 * user to remember which chords are dangerous:
 *
 *     hold  Y + N + P + /   (the four corners, positions 0 4 10 14)
 *         -> enter: K blinks BLUE twice
 *         -> leave: K blinks RED  twice
 *
 * How the "disable" is implemented
 * --------------------------------
 * Not by making the behaviours no-ops - that would still leave each combo
 * capturing its keys (ZMK suppresses a combo's member keys while it is a
 * candidate), so the notes would still vanish. Pure mode instead switches the
 * keymap to a second layer whose fifteen bindings are the same `&user_kp`
 * notes, and every function combo is declared `layers = <0>` in the keymap.
 * Combo candidate selection is gated on the HIGHEST ACTIVE layer, so on layer 1
 * no combo is even a candidate: keys pass straight through, no capture, no
 * 80 ms combos timeout, and the notes still type.
 *
 * The gesture itself is therefore NOT a ZMK combo (it could not be: it costs
 * no capture exactly where we need four keys of arbitrary timing). It is
 * detected in pure_mode.c from position events, which are dispatched to module
 * listeners before the combo engine sees them.
 */
#pragma once
#include <stdbool.h>

/* Keymap layer that carries the pure bindings (the second layer in the map).
 * Layer ids are devicetree order for a static keymap. */
#define PURE_MODE_LAYER 1u

/* K - the middle key of the pad, keymap position 7. The strip is wired in
 * keymap order (one pixel per key), so the LED index is the same number. */
#define PURE_MODE_BLINK_KEY 7u

/* Idempotent. Called once from module_init.c. */
void pure_mode_init(void);

/* True while pure mode is engaged. Read by the function behaviours as a second
 * belt behind the layer switch (guards still hold if the keymap is edited and
 * a combo loses its `layers` filter). */
bool pure_mode_is_active(void);

/* Flip the mode now, without waiting for a gesture. Exposed for diagnostics. */
void pure_mode_toggle(void);
