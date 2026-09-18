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
  step-by-step; the keyboard lights the current key BLUE and the next key RED.
  Strictly user-paced - only a correct key press moves the cursor: correct
  press flashes GREEN, then arms the next step after that step's delta; wrong
  press flashes ALL RED (MISS) and keeps waiting on the same step; doing
  nothing only pulses the expected key AMBER (1200-8000 ms). Nothing
  auto-advances, so the 0x03 TIMEOUT event is never emitted
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
| 0x31 MODE_C_PUSH  | u16 delta_ms, u8 key(1..15), u8 duration_ms | push one step |
| 0x32 MODE_C_TICK  | u32 app_ms | reference clock (informational) |
| 0x35 MODE_C_STOP  | -- | abort |

BLE EVENTS characteristic (0xBE05, NOTIFY) payload = 4 bytes:
`[event_code, key, step_lo, step_hi]`

- 0x01 HIT (key = correct key), 0x02 MISS (key = wrong key),
  0x03 TIMEOUT (key = 0), 0x04 DONE (key = 0)

0x03 stays in the protocol so older app builds need no change, but it is no
longer emitted: a step waits for the correct key indefinitely rather than
timing out and skipping. `duration_ms` is now only the reminder-pulse interval
(floored at 1200 ms, capped at 8000 ms) - not a deadline.

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
   - Push every step with 0x31 PUSH (delta_ms, key 1..15, duration_ms)
   - Send 0x30 START with the step count
   - Current step's key lights BLUE, next step's key lights RED
   - Press the BLUE key -> it flashes GREEN and after this step's delta the
     next step arms; App receives HIT
   - Press a wrong key -> all LEDs flash RED, App receives MISS, and the step
     stays put - a wrong press never advances
   - Wait -> the expected key pulses AMBER every duration_ms (1200-8000 ms) as
     a reminder and nothing else happens. The cursor only moves on a correct
     press, so no TIMEOUT event is sent
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
  mode_c, module_init) + behaviors/ 5 C files