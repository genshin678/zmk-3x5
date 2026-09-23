# zmk-rgb-player

Custom ZMK module for the **ZMK 3x5 Bluetooth Keyboard** shield.

Provides:

- WS2812B per-pixel LED driver (15 LEDs, 1 per key) on D8 (P1.04, SPI3 MOSI)
- 7 RGB effects (cycled by the 4-corner combo):
  - 0 OFF
  - 1 静态单色    (ZMK built-in solid)
  - 2 单色呼吸    (ZMK built-in breathing)
  - 3 彩虹循环    (ZMK built-in spectrum)
  - 4 单点亮      (custom per-pixel)
  - 5 星光闪烁    (custom per-pixel)
  - 6 波纹        (custom per-pixel, wave from last key)
- Score playback engine: load via USB CDC or BLE GATT, auto-press + light
  keys in mode B (mode A = light only)
- **Mode C (Assisted Play-Along / 引导弹奏):** phone App pushes the score
  step-by-step; the keyboard lights the step's keys BLUE and the next step's
  keys RED. **A step is a chord, not a note:** it carries a 15-bit key mask and
  is satisfied once every key in it has been pressed (in any order - a rolled
  chord counts), so one step is one cell of the printed sheet, i.e. one 小节
- **Mode C pacing is switchable at run time** (0x36 MODE_C_PACE, mid-session
  included, in both directions). MANUAL waits for the user and nothing advances
  on a clock: a correct press flashes GREEN then arms the next step after that
  step's delta, a wrong press flashes ALL RED (MISS) and stays put, and a step
  simply keeps waiting - there is no amber pulse any more. AUTO walks the score
  itself, does not judge presses, and **actually plays the game**: every step it
  arms is emitted as real USB-HID keycodes (all keys of the mask together, held
  50-200 ms then lifted), reusing the same HID engine mode B uses, so the notes
  land in the host/game and the LEDs just follow along. The 0x03 TIMEOUT event is
  never emitted in either pace
- **All combos are locked while Mode C runs** (Y+P brightness, H+; hue,
  Y+N+P+/ effect cycle, U+M+O+. bond clear, I+K transport toggle, P+N DFU), via a
  `mode_c_is_active()` guard at the top of each behaviour. The play-along cue owns
  the LED strip and the score owns those keys, so a combo pressed mid-run can no
  longer push the blue/red cue off the LEDs or flip the transport out from under
  the notes. They come back the instant the run ends (DONE) or you stop it (0x35
  STOP). To force DFU mid-song, double-tap RESET - that path is hardware and is
  never gated
- Custom BLE GATT service for the mobile app:
  - 0xBE01 Score Upload (WRITE)
  - 0xBE02 Playback Control (WRITE: PLAYER A/B + Mode C 0x30-0x35)
  - 0xBE03 Live Keypress (WRITE)
  - 0xBE04 Status (READ + NOTIFY)
  - 0xBE05 Mode C Events (NOTIFY: HIT/MISS/DONE; 0x03 TIMEOUT reserved, never sent)
- USB CDC debug console (PING, PLAY, MODE A, MODE B, LOAD:n, BRIGHTNESS v,
  HUE v, EFFECT n, REBOOT DFU)
- DFU trigger: 4-corner combo Y+N+P+/ cycles effect, diagonal P+N reboots
  into UF2 bootloader
- Keymap: inner keys send their letter + trigger ripple; corner keys are
  &none (used by RGB-control combos)

## Mode C protocol (Assisted Play-Along)

BLE CONTROL characteristic (0xBE02):

| cmd | params | meaning |
|-----|--------|---------|
| 0x30 MODE_C_START | u16 note_count | begin (after all steps pushed) |
| 0x31 MODE_C_PUSH  | u16 delta_ms, u16 key_mask, u8 duration_ms | push one step (a chord) |
| 0x32 MODE_C_TICK  | u32 app_ms | reference clock (informational) |
| 0x35 MODE_C_STOP  | -- | abort playback **and clear the step table** |
| 0x36 MODE_C_PACE  | u8 pace (0 = manual, 1 = auto) | switch pacing, any time |

BLE EVENTS characteristic (0xBE05, NOTIFY) payload = 4 bytes:
`[event_code, key, step_lo, step_hi]`

- 0x01 HIT (key = lowest key of the satisfied chord), 0x02 MISS (key = the
  wrong key pressed), 0x03 TIMEOUT (never sent), 0x04 DONE (key = 0),
  0x05 STEP (key = how many keys the step wants, step = its index)

0x05 STEP is emitted every time a step arms, in **both** paces. In manual pace it
is a second opinion on the cursor; in auto pace it is the only one there is -
auto never emits HIT, so a companion display deriving the position from HITs would
sit on cell 0 forever.

0x03 stays in the protocol so older app builds need no change, but it is no longer
emitted: a step waits for the user indefinitely rather than timing out and
skipping. `duration_ms` is now purely informational - nothing times out at all.

`key_mask` is bit(k-1) for key k in 1..15 and must be non-zero. Bit 15 (key 16) is
rejected rather than ignored: the renderer has no pixel for it, so a step carrying
it could never be satisfied and would hang the run with no error. Rests are not
steps - rest time folds into the preceding step's `delta`.

The step table costs 2048 x 5 bytes = 10 KiB of RAM (a packed struct of
u16 delta, u16 mask, u8 duration), up from 8 KiB when a step was one key.

Firmware capability is advertised in BE04 status byte 8: bit0 = wide delta (delta
not clamped to 1 s on a HIT), bit1 = chord mask (this 6-byte PUSH layout), bit2 =
auto pace plays the game (AUTO emits real HID keycodes; firmware without it only
animates the LEDs). Note that
firmware *without* bit1 does not reject a 6-byte PUSH - it parses it with the old
5-byte layout and lights a wrong chord with no error at all - so a client has to
keep sending single keys until the bit appears.

All 15 physical keys are detected via the ZMK position event, including the
four corners that are bound to &none in the keymap.

## Build

`
cd C:\Users\13983\.qclaw\workspace\zmk-3x5-bt
git add -A
git commit -m "P1+P2+P3 + RGB + DFU"
git push origin main
`

GitHub Actions (.github/workflows/build.yml) builds the firmware on push.
Download the resulting .uf2 and drag it to the nice!nano USB drive.

## Test

1. Flashing
   - Double-tap the reset button on nice!nano, it appears as a USB drive
   - Drag the .uf2 to the drive
   - Keyboard reboots, all 15 LEDs should light up in the default effect

2. Manual LED control (verify P1)
   - Press a key: that key's LED lights up (single-key effect)
   - Hold Y: brightness ramps down
   - Hold P: brightness ramps up
   - Hold H: hue decreases
   - Hold ;: hue increases
   - Press all 4 corners (Y+N+P+/) at once: effect cycles

3. DFU trigger (verify P1)
   - Press P+N together (within 200ms): keyboard reboots into UF2
   - Confirm it reappears as a USB drive

4. USB CDC score load (verify P2)
   - Connect via serial (115200 8N1) on the hardware UART or USB CDC
   - Send: PING -> PONG
   - Send: EFFECT 0 (switch to solid)
   - Send: MODE B (auto-press mode)
   - Send: BRIGHTNESS 200
   - Pipe a binary score via LOAD:n, then PLAY. The keyboard should
     auto-press the keys and light the LEDs in sequence.

5. BLE GATT (verify P3, needs phone app)
   - Use nRF Connect on Android
   - Scan, find the keyboard, connect
   - Discover services: should see custom service
     9e3c1b0a-7a4b-4f0e-8d1d-7a6f5c3b2a10
   - 0xBE01 Score Upload (WRITE)
   - 0xBE02 Playback Control (WRITE)
   - 0xBE04 Status (READ + NOTIFY)
   - 0xBE05 Mode C Events (NOTIFY)

6. Mode C (verify P2.1, needs phone app)
   - Connect, enable NOTIFY on 0xBE04 and 0xBE05
   - Send 0x35 STOP first: it clears the step table. Skipping it makes a second
     run append to the first and play both scores
   - Push every step with 0x31 PUSH (delta_ms, key_mask, duration_ms). All keys of
     one sheet cell travel in a single PUSH - bit(k-1) for key k
   - Send 0x36 PACE 0 (manual), then 0x30 START with the step count
   - The step's keys light BLUE, the next step's keys light RED; a key wanted by
     both stays BLUE. App receives STEP for every arming
   - Press every BLUE key, in any order (they need not be simultaneous) -> they
     flash GREEN and after this step's delta the next step arms; App receives HIT
   - Press a wrong key -> all LEDs flash RED, App receives MISS, and the step stays
     put - a wrong press never advances, and the keys of the chord that were already
     accepted are kept, so one slip does not force a full restart
   - Wait -> nothing happens and the blue cue simply stays. No amber pulse, no
     TIMEOUT event
   - Send 0x36 PACE 1 mid-run -> the keyboard walks the score by itself from the step
     currently on screen **and plays it into the game** (real HID keycodes appear on the
     host); 0x36 PACE 0 hands it back to you
   - While it runs, press Y+N+P+/ (effect cycle) or H+; (hue): the combos are inert and
     the blue/red cue stays put. Stop the run and they work again
   - After the last step, LEDs go dark, App receives DONE

## Known TODO

- and led_pixel behavior is defined but not used in the default keymap;
  it is the low-level API for the player / effects.
- The dan-dian-liang effect needs a single_key_light_N wrapper behavior
  per key for the keymap to feed effects_set_active_key(). Not done yet,
  the effect is currently a no-op until the player runs.
- ble_service.c on_keypress_write lights the LED but does not schedule
  the auto-clear (k_msleep in a GATT callback is unsafe). Add a work item.
- Score load via BLE is append-only; if a previous load is pending you
  must write 0x20 (CLEAR) to the control characteristic first.
- The WS2812B driver (worldsemi,ws2812-sspi) requires SPI3 to be enabled
  and pinctrl configured. The shield overlay does this; if your board
  variant differs, adjust the pinctrl in boards/shields/zmk_3x5_bt/.

## Files

modules/zmk-rgb-player/
- CMakeLists.txt
- zephyr/ (Kconfig, module.yml, dts/bindings/*.yaml)
- include/zmk_rgbeffect/ (7 headers: led_pixel, effects, player, ble_service,
  rgb_control, dfu, mode_c)
- src/ (led_pixel, effects, player, ble_service, rgb_control, dfu, cdc,
  mode_c, module_init) + behaviors/ 7 C files (incl. behavior_out_guard.c)