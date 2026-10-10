/*
 * cdc.c - USB CDC command channel: let a PC drive the board over the cable.
 *
 * WHY THIS EXISTS
 * ---------------
 * Every other transport this board understands (BE01/BE02 GATT, the phone App)
 * ends at a Bluetooth link. Plug the board into a PC with a USB cable and the
 * phone is out of the picture - but the keyboard side keeps working, because
 * ZMK's USB output is still a real HID keyboard on the other end. What the
 * cable was missing is a way to get a SCORE INTO the board while it is plugged
 * in. That is this file: a line-oriented command channel over the USB CDC ACM
 * function.
 *
 * It is deliberately TEXT, not binary opcodes. GATT has framing (write with
 * offset, MTU negotiation, a read to confirm); a serial port has none, so the
 * only protocol that survives 115200 baud in both directions with a terminal
 * window attached is something a human can read. The one binary command is
 * LOAD:, because a two-minute score is several hundred steps and typing them
 * is not an option.
 *
 * The older version of this file guarded on `zmk,console`, a chosen node this
 * board never declares, and was additionally never called from module_init().
 * It compiled to nothing and did nothing. It is now wired to `zmk,pc-uart` and
 * called from module_init().
 *
 * CONNECTION
 * ----------
 * chosen `zmk,pc-uart` in the shield overlay names the cdc_acm0 node. Without
 * it the whole translation unit compiles to nothing (the event hook collapses
 * to a no-op instead, so ble_service.c can call it unconditionally).
 *
 * STEPS
 * -----
 * One step is 5 bytes, little-endian, identical to the payload of GATT 0x31
 * MODE_C_PUSH after the opcode:
 *
 *     u16 delta_ms  |  u16 key_mask  |  u8 duration_ms
 *
 * `key_mask` bit (k-1) is key k in 1..15.
 *
 * PROTOCOL (PC -> board, one command per line, LF terminated)
 * -----------------------------------------------------------
 *   PING                        -> PONG,<ver>-EVTGATE
 *   LOAD:<nbytes>               -> exactly n raw bytes follow on the wire
 *   PUSH <delta> <mask> <dur>   append one step (debug / step-by-step)
 *   START [<count>]             mode_c_start(count); count defaults to table size
 *   STOP                        mode_c_stop() - aborts and clears the table
 *   PAUSE                       reports where the cursor sits
 *   PACE <0|1>                  0 = manual (user paced), 1 = auto (board plays)
 *   TICK <ms>                   reference clock, informational
 *   KEYS                        read back the 15-key map
 *   KEYS:<hex>                  15 one-byte HID usages as 30 hex chars
 *   KEYRESET                    restore the factory 光遇 layout
 *   LIGHT                       read back effect,brightness,hue
 *   EFFECT <0..6>               LED effect
 *   BRIGHT <0..96>              96 is the ceiling: 15 LEDs on a 5 V rail
 *   HUE <0..359>                hue
 *   LIGHTSAVE                   persist effect/brightness/hue to NVS
 *   STATUS                      one-line dump of everything above
 *   REBOOT                      back to DFU / reboot
 *
 * PROTOCOL (board -> PC, one answer or event per line)
 * ----------------------------------------------------
 *   PONG,<ver>-EVTGATE
 *   OK <cmd> [<detail>]
 *   ERR <cmd> <reason>
 *   EVT:HIT,<key>,<step>
 *   EVT:MISS,<key>,<step>
 *   EVT:DONE,<step_count>
 *   EVT:STEP,<keys_in_chord>,<step_index>
 *   EVT:HIDFAIL,<streak>,<total>
 *   INFO:<text>
 *
 * EVT:* lines come from cdc_emit_event(), called from ble_service.c's
 * mode_c_notify_event() so a wired session reports to the PC exactly what a
 * wireless one reports to the phone. They are the only way a client can follow
 * the cursor in AUTO pace, where nothing is ever HIT.
 *
 * THREADING
 * ---------
 * UART interrupt callbacks must not call into mode_c / player / rgb_control:
 * they take locks and reschedule the HID writers. Every command is copied into
 * a buffer and dispatched from a k_work on the system workqueue. The one
 * exception is cdc_emit_event(), which only ever does uart_poll_out() of an
 * already-formatted line.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/spinlock.h>
/* No work.h include here on purpose: this Zephyr's <zephyr/kernel.h> already
 * pulls the work queue declarations in, and there is no <zephyr/work.h> header
 * in this tree - asking for it is a hard "No such file or directory". */
#include <zmk_rgbeffect/mode_c.h>
#include <zmk_rgbeffect/player.h>
#include <zmk_rgbeffect/rgb_control.h>
#include <zmk_rgbeffect/user_keymap.h>
#include <zmk_rgbeffect/dfu.h>

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

#define PC_LINE_MAX     160   /* longest command line we accept */
#define PC_TX_MAX       192   /* longest answer / event line we emit */
#define PC_STEP_BYTES   5     /* u16 delta LE | u16 mask LE | u8 dur */
#define PC_PROTOCOL_VER 1
/* Carrying a build tag, not just a counter: the EVT-gate fix changed runtime
 * behaviour only, so "did my UF2 actually get flashed into the keyboard?" had
 * no observable answer from the PC side. PING -> PONG,1-EVTGATE is cheap,
 * greppable proof either way, and greppable in the UF2 itself. */
#define PC_PROTOCOL_TAG "EVTGATE"

/* Everything below is compiled out unless the shield names a UART for it AND the
 * serial driver is actually in the build.
 *
 * The second half matters: DEVICE_DT_GET(DT_CHOSEN(zmk_pc_uart)) resolves to a
 * symbol the ACM driver provides. With CONFIG_SERIAL or CONFIG_USB_CDC_ACM off
 * that driver is not built, so referencing the device compiles fine and dies at
 * link time with an undefined __device_dts_ord_N - a much worse diagnostic than
 * a #if that simply drops the file. */
#if DT_HAS_CHOSEN(zmk_pc_uart) && IS_ENABLED(CONFIG_SERIAL) && IS_ENABLED(CONFIG_USB_CDC_ACM)

/* ---- command queue, dispatched on the system workqueue ----
 *
 * Lines arrive one character at a time from the UART ISR, which drains the
 * whole received chunk in a single callback. A USB CDC packet is ~64 bytes and
 * easily contains several complete STEP lines, so one uart_cb() can hand us
 * three or four newlines back to back. With a single shared buffer the second
 * line overwrites the first before the workqueue runs, and resubmitting an
 * already-pending k_work is a silent no-op (returns -EBUSY) - so the
 * overwritten command is simply lost. That is what used to drop most of a
 * several-hundred-step score.
 *
 * Fix: each finished line is copied into a ring slot and the work just drains
 * the ring to empty, resubmitting itself if more lines land while it is
 * running. Nothing is dropped for "being too fast" short of the ring filling,
 * which at 115200 baud would need >PC_LINE_RING lines inside one USB packet -
 * far beyond the CDC ACM RX buffer. */
static char    cmd_buf[PC_LINE_MAX];     /* per-character assembler */
static size_t  cmd_len;
#define PC_LINE_RING  64
static char    cmd_ring[PC_LINE_RING][PC_LINE_MAX];
static uint8_t cmd_ring_head;
static uint8_t cmd_ring_tail;
static uint8_t cmd_ring_count;
static struct k_spinlock cmd_ring_lock;
static struct k_work_delayable cmd_work;

/* A whole score waiting to become steps. Separate from cmd_buf because a
 * binary payload has no line structure to accumulate into. */
static uint8_t  load_buf[MODE_C_MAX_STEPS * PC_STEP_BYTES];
static uint16_t load_len;   /* bytes received so far */
static uint16_t load_want;  /* bytes the LOAD: header promised */
static bool     loading;    /* between the header and the payload */

/* ---- output ----
 *
 * Lines are queued, never written directly. uart_poll_out() on a cdc_acm
 * device takes the driver's mutex and blocks on the USB write path - and this
 * function is called from that very device's own interrupt callback (the UART
 * callback below, and cdc_emit_event() firing inside mode_c's HID work), so
 * writing straight from here would recurse into a lock the ISR already holds.
 * The ring therefore absorbs writes from interrupt context and a workqueue
 * drains it; if the ring is full the line is dropped, because a reply that
 * arrives late is worth less than the 300 us spent blocking the radio. */

#define PC_TX_SLOTS   8
/* +2 so a filled slot always has room for the terminating newline and the NUL
 * even when the formatted line exactly fills PC_TX_MAX - 1 characters. */
#define PC_TX_ROW     (PC_TX_MAX + 2)

/* k_spinlock_key_t became a struct in this Zephyr, so the key has to be typed
 * with it rather than with unsigned int; the lock itself is still a plain
 * struct here - there is no K_SPINLOCK_DEFINE macro in this tree. */
static struct k_spinlock tx_lock;
static uint8_t  tx_pool[PC_TX_SLOTS][PC_TX_ROW];
static uint8_t  tx_tail;      /* next slot a producer may write */
static uint8_t  tx_count;
static uint32_t tx_dropped;
/* How many events reached cdc_emit_event(), and how many output slots were
 * dropped because the ring was full. Both are reported by STATUS so a PC
 * session can tell 'firmware never produced it' from 'the wire ate it'. */
static volatile uint32_t pc_evt_seen;
static volatile uint32_t pc_tx_dropped;

static void pc_tx_drain(struct k_work *work);

K_WORK_DEFINE(pc_tx_work, pc_tx_drain);

static void pc_tx_raw(const char *s, size_t n) {
    const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zmk_pc_uart));
    if (!device_is_ready(dev)) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        uart_poll_out(dev, s[i]);
    }
}

/* The only place that touches the wire. Runs on the system workqueue, so it
 * is never inside the cdc_acm device's own interrupt context. */
static void pc_tx_drain(struct k_work *work) {
    (void)work;
    for (;;) {
        k_spinlock_key_t key = k_spin_lock(&tx_lock);
        if (tx_count == 0) {
            k_spin_unlock(&tx_lock, key);
            return;                 /* also the signal that we stopped pending */
        }
        uint8_t slot = tx_tail;
        uint8_t *buf = tx_pool[slot];
        k_spin_unlock(&tx_lock, key);

        size_t n = strnlen((const char *)buf, PC_TX_MAX);
        pc_tx_raw((const char *)buf, n);
        if (n > 0 && buf[n - 1] != '\n') {
            pc_tx_raw("\n", 1);
        }

        k_spinlock_key_t key2 = k_spin_lock(&tx_lock);
        tx_pool[slot][0] = 0;
        tx_tail = (uint8_t)((tx_tail + 1) % PC_TX_SLOTS);
        tx_count--;
        k_spin_unlock(&tx_lock, key2);
    }
}

/* Safe from interrupt context: the ring and a work submit, nothing more. */
static void pc_tx(const char *fmt, ...) {
    char *dst = (char *)tx_pool[(uint8_t)(tx_tail + tx_count) % PC_TX_SLOTS];
    va_list ap;
    va_start(ap, fmt);

    k_spinlock_key_t key = k_spin_lock(&tx_lock);
    bool full = (tx_count >= PC_TX_SLOTS);
    k_spin_unlock(&tx_lock, key);

    if (full) {
        va_end(ap);
        tx_dropped++;              /* newest line is the least interesting */
        pc_tx_dropped++;
        return;
    }

    int n = vsnprintf(dst, PC_TX_ROW - 1, fmt, ap);
    va_end(ap);
    if (n <= 0 || (size_t)n >= PC_TX_ROW - 1) {
        dst[0] = 0;
        return;
    }
    dst[n] = '\n';
    dst[n + 1] = 0;

    k_spinlock_key_t key2 = k_spin_lock(&tx_lock);
    tx_count++;
    uint8_t was_empty = (tx_count == 1);
    k_spin_unlock(&tx_lock, key2);

    if (was_empty) {
        (void)k_work_submit(&pc_tx_work);
    }
}

/* ---- input helpers ---- */

/* 30 hex chars -> 15 bytes, written straight into the keymap table. */
static bool pc_parse_hex(const char *s, uint8_t *out, int count) {
    if (!s || strlen(s) < (size_t)count * 2) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        char tmp[3] = { s[i * 2], s[i * 2 + 1], 0 };
        char *end = NULL;
        long v = strtol(tmp, &end, 16);
        if (end != tmp + 2 || v < 0 || v > 0xFF) {
            return false;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

static void pc_print_keymap(void) {
    pc_tx("KEYS:");
    for (int i = 0; i < USER_KEYMAP_SLOTS; i++) {
        pc_tx("%02X", user_keymap_get_slot((uint8_t)i));
    }
}

static void pc_print_light(void) {
    pc_tx("LIGHT:%u,%u,%u", rgb_control_get_effect(),
          rgb_control_get_brightness(), rgb_control_get_hue());
}

/* ---- command dispatch ---- */

static void pc_load_work_fn(struct k_work *work);
static void pc_cmd_work_fn(struct k_work *work);
static void pc_load_payload_done(void);
static void pc_cmd_load_header(uint16_t want);

/* Parse and execute one already-completed command line. Pulled out of
 * pc_cmd_work_fn so the work can loop over the ring without re-entering the
 * parser; it receives a private, NUL-terminated copy that nothing else mutates,
 * so concurrent uart_cb() writers can keep filling the next ring slot safely. */
static void pc_exec_line(char *buf) {
    char *save = NULL;
    char *tok = strtok_r(buf, " \t\r\n", &save);
    if (tok == NULL) {
        return;
    }
    const char *cmd = tok;
    uint16_t a = 0;
    uint16_t b = 0;
    uint8_t  c = 0;

    if (strcmp(cmd, "PING") == 0) {
        pc_tx("PONG,%u-%s", PC_PROTOCOL_VER, PC_PROTOCOL_TAG);

    } else if (strcmp(cmd, "LOAD:") == 0) {
        tok = strtok_r(NULL, " \t\r\n", &save);
        uint32_t want = tok ? (uint32_t)strtoul(tok, NULL, 10) : 0;
        if (want == 0 || want % PC_STEP_BYTES != 0 || want > sizeof(load_buf)) {
            pc_tx("ERR LOAD bad-length %lu", (unsigned long)want);
        } else {
            pc_cmd_load_header((uint16_t)want);
        }

    } else if (strcmp(cmd, "PUSH") == 0) {
        tok = strtok_r(NULL, " \t\r\n", &save);
        a = tok ? (uint16_t)strtoul(tok, NULL, 10) : 0;
        tok = strtok_r(NULL, " \t\r\n", &save);
        b = tok ? (uint16_t)strtoul(tok, NULL, 10) : 0;
        tok = strtok_r(NULL, " \t\r\n", &save);
        c = tok ? (uint8_t)strtoul(tok, NULL, 10) : 0;
        mode_c_stop();
        mode_c_push(a, b, c);
        pc_tx("OK PUSH %u %u %u", a, b, c);

    } else if (strcmp(cmd, "STEP") == 0) {
        /* Append one step and leave playback alone. This is the wired form of
         * GATT 0x31 MODE_C_PUSH: the PC client translates every PUSH frame into
         * a STEP line, because 0x31's own handler stops first and a translated
         * PUSH would abort after the first step of a several-hundred-step
         * score. */
        tok = strtok_r(NULL, " \t\r\n", &save);
        a = tok ? (uint16_t)strtoul(tok, NULL, 10) : 0;
        tok = strtok_r(NULL, " \t\r\n", &save);
        b = tok ? (uint16_t)strtoul(tok, NULL, 10) : 0;
        tok = strtok_r(NULL, " \t\r\n", &save);
        c = tok ? (uint8_t)strtoul(tok, NULL, 10) : 0;
        mode_c_push(a, b, c);
        pc_tx("OK STEP %u %u %u", a, b, c);

    } else if (strcmp(cmd, "START") == 0) {
        tok = strtok_r(NULL, " \t\r\n", &save);
        uint16_t n = tok ? (uint16_t)strtoul(tok, NULL, 10) : mode_c_step_count();
        if (n == 0) {
            n = mode_c_step_count();
        }
        mode_c_start(n);
        pc_tx("OK START %u", n);

    } else if (strcmp(cmd, "STOP") == 0) {
        mode_c_stop();
        pc_tx("OK STOP");

    } else if (strcmp(cmd, "PAUSE") == 0) {
        pc_tx("OK PAUSE %u", mode_c_current_step());

    } else if (strcmp(cmd, "PACE") == 0) {
        tok = strtok_r(NULL, " \t\r\n", &save);
        mode_c_set_pace(tok && strcmp(tok, "1") == 0 ? MODE_C_PACE_AUTO
                                                     : MODE_C_PACE_MANUAL);
        pc_tx("OK PACE %u", mode_c_get_pace());

    } else if (strcmp(cmd, "TICK") == 0) {
        tok = strtok_r(NULL, " \t\r\n", &save);
        mode_c_tick(tok ? (uint32_t)strtoul(tok, NULL, 10) : 0);
        pc_tx("OK TICK");

    } else if (strcmp(cmd, "KEYS") == 0) {
        tok = strtok_r(NULL, " \t\r\n", &save);
        if (tok == NULL) {
            pc_print_keymap();
        } else if (pc_parse_hex(tok, user_keymap_table, USER_KEYMAP_SLOTS)) {
            user_keymap_save();
            pc_print_keymap();
        } else {
            pc_tx("ERR KEYS bad-usage-string");
        }

    } else if (strcmp(cmd, "KEYRESET") == 0) {
        user_keymap_reset_defaults();
        pc_print_keymap();

    } else if (strcmp(cmd, "LIGHT") == 0) {
        pc_print_light();

    } else if (strcmp(cmd, "EFFECT") == 0) {
        tok = strtok_r(NULL, " \t\r\n", &save);
        rgb_control_set_effect(
            (rgb_effect_t)(tok ? (uint8_t)strtoul(tok, NULL, 10) : 0));
        pc_print_light();

    } else if (strcmp(cmd, "BRIGHT") == 0) {
        tok = strtok_r(NULL, " \t\r\n", &save);
        rgb_control_set_brightness(
            (uint8_t)(tok ? strtoul(tok, NULL, 10) : 0));
        pc_print_light();

    } else if (strcmp(cmd, "HUE") == 0) {
        tok = strtok_r(NULL, " \t\r\n", &save);
        rgb_control_set_hue((uint16_t)(tok ? strtoul(tok, NULL, 10) : 0));
        pc_print_light();

    } else if (strcmp(cmd, "LIGHTSAVE") == 0) {
        rgb_control_save();
        pc_tx("OK LIGHTSAVE");

    } else if (strcmp(cmd, "STATUS") == 0) {
        /* fw= echoes the compiled-in capability bits; usbevt= says whether the CDC
         * event path exists in this build at all. Between them they settle the two
         * questions the PC kept guessing at: is this the build I flashed, and can it
         * emit EVT:* lines on the wire? */
        pc_tx("STATUS:pace=%u,steps=%u,active=%u,link=%u/%u,fw=%u,usbevt=%u,evt=%u,drop=%u",
              mode_c_get_pace(), mode_c_step_count(),
              mode_c_is_active() ? 1 : 0,
              mode_c_link_endpoint(), mode_c_link_usb_state(),
              (unsigned)ZMK_RGB_PLAYER_FW_FLAGS,
              (DT_HAS_CHOSEN(zmk_pc_uart) && IS_ENABLED(CONFIG_SERIAL) &&
               IS_ENABLED(CONFIG_USB_CDC_ACM)) ? 1u : 0u,
              (unsigned)pc_evt_seen, (unsigned)pc_tx_dropped);
        pc_print_light();

    } else if (strcmp(cmd, "REBOOT") == 0) {
        pc_tx("OK REBOOT");
        dfu_request();

    } else {
        pc_tx("ERR %s unknown-command", cmd);
    }
}

/* Drain every pending command. Runs on the system workqueue: pop one ring slot,
 * execute it, and if more lines arrived while we were busy, resubmit so the
 * workqueue keeps draining until the ring is empty. A k_work submitted while
 * already pending is a silent no-op, so we must re-arm from inside the loop
 * rather than rely on the submit at enqueue time. */
static void pc_cmd_work_fn(struct k_work *work) {
    (void)work;
    for (;;) {
        char *line;
        k_spinlock_key_t key = k_spin_lock(&cmd_ring_lock);
        if (cmd_ring_count == 0) {
            k_spin_unlock(&cmd_ring_lock, key);
            return;
        }
        line = cmd_ring[cmd_ring_tail];
        cmd_ring_tail = (uint8_t)((cmd_ring_tail + 1) % PC_LINE_RING);
        cmd_ring_count--;
        k_spin_unlock(&cmd_ring_lock, key);

        pc_exec_line(line);

        k_spinlock_key_t key2 = k_spin_lock(&cmd_ring_lock);
        bool more = (cmd_ring_count > 0);
        k_spin_unlock(&cmd_ring_lock, key2);
        if (more) {
            (void)k_work_submit(&cmd_work);
        }
    }
}

/* A binary payload is complete; turn it into steps on the workqueue. */
static void pc_load_work_fn(struct k_work *work) {
    (void)work;
    const uint8_t *p = load_buf;
    uint16_t total = load_want;
    uint16_t n = (uint16_t)(total / PC_STEP_BYTES);

    if (total % PC_STEP_BYTES != 0) {
        pc_tx("ERR LOAD ragged %u", total);
        return;
    }
    mode_c_stop();
    for (uint16_t i = 0; i < n; i++) {
        uint16_t delta = (uint16_t)(p[0] | (p[1] << 8));
        uint16_t mask  = (uint16_t)(p[2] | (p[3] << 8));
        uint8_t  dur   = p[4];
        p += PC_STEP_BYTES;
        mode_c_push(delta, mask, dur);
    }
    pc_tx("OK LOAD %u steps", n);
    load_want = 0;
    loading = false;
}

K_WORK_DEFINE(pc_load_work, pc_load_work_fn);

/* ---- interrupt-side input ---- */

static void pc_newline(void) {
    if (cmd_len == 0) {
        return;     /* empty line: nothing to enqueue */
    }
    cmd_buf[cmd_len] = 0;

    /* Copy the finished line into a ring slot. The work drains the ring fully,
     * so we only need to submit when it was empty (work not already pending); if
     * it is pending/running it will pick this slot up on its next loop pass.
     * The memcpy + cmd_len reset happen inside the spinlock: on this single-core
     * target k_spin_lock() masks the UART ISR, so pc_byte cannot append a char
     * to cmd_buf mid-copy and split one line across two slots. */
    char *slot;
    bool was_empty;
    k_spinlock_key_t key = k_spin_lock(&cmd_ring_lock);
    if (cmd_ring_count >= PC_LINE_RING) {
        k_spin_unlock(&cmd_ring_lock, key);
        cmd_len = 0;
        pc_tx("ERR cmd-queue-overflow");
        return;
    }
    slot = cmd_ring[cmd_ring_head];
    cmd_ring_head = (uint8_t)((cmd_ring_head + 1) % PC_LINE_RING);
    was_empty = (cmd_ring_count == 0);
    cmd_ring_count++;
    memcpy(slot, cmd_buf, cmd_len + 1);
    cmd_len = 0;
    k_spin_unlock(&cmd_ring_lock, key);

    if (was_empty) {
        /* k_work_submit_delayable() is deliberately avoided: in this Zephyr the
         * call compiled as an implicit declaration and then died at link time
         * with "undefined reference to `k_work_submit_delayable'". cmd_work is a
         * plain (non-delayable) K_WORK_DEFINE and the delay was always K_MSEC(0),
         * so submitting it outright is exactly the same thing and uses an API the
         * rest of the module already leans on. */
        if (k_work_submit(&cmd_work) < 0) {
            pc_tx("ERR cmd-queue-busy");
        }
    }
}

static void pc_load_payload_done(void) {
    (void)k_work_submit(&pc_load_work);
}

static void pc_cmd_load_header(uint16_t want) {
    load_want = want;
    load_len = 0;
    loading = true;
}

static void pc_byte(uint8_t ch) {
    if (loading) {
        if (load_len < load_want) {
            load_buf[load_len++] = ch;
        }
        if (load_len >= load_want) {
            pc_load_payload_done();
        }
        return;
    }
    if (ch == '\n' || ch == '\r') {
        if (cmd_len > 0) {
            pc_newline();
        }
        return;
    }
    if (cmd_len < PC_LINE_MAX - 1) {
        cmd_buf[cmd_len++] = (char)ch;
    } else {
        cmd_len = 0;   /* overlong line: drop it, never overflow into the next */
    }
}

static void uart_cb(const struct device *dev, void *user_data) {
    ARG_UNUSED(user_data);
    uint8_t ch;
    while (uart_poll_in(dev, &ch) == 0) {
        pc_byte(ch);
    }
}

#else /* !DT_HAS_CHOSEN(zmk_pc_uart) || no serial/ACM in the build */

/* Neither is ever referenced in this configuration; the attribute keeps the
 * "defined but not used" warning from becoming build noise. */
__attribute__((unused)) static void pc_load_payload_done(void) {}
__attribute__((unused)) static void pc_cmd_load_header(uint16_t want) {
    ARG_UNUSED(want);
}

#endif /* DT_HAS_CHOSEN(zmk_pc_uart) && CONFIG_SERIAL && CONFIG_USB_CDC_ACM */

/* ---- events -> the cable ----
 *
 * Called from ble_service.c's mode_c_notify_event() for every mode_c event, so
 * a wired session reports to the PC exactly what a wireless one reports to the
 * phone. Declared here as a non-static symbol with the same name in both
 * build configurations; ble_service.c must not branch on it. */

#if DT_HAS_CHOSEN(zmk_pc_uart) && IS_ENABLED(CONFIG_SERIAL) && IS_ENABLED(CONFIG_USB_CDC_ACM)

void cdc_emit_event(uint8_t event, uint8_t key, uint16_t step) {
    /* Counting every event that actually reaches the USB exit. After several
     * rounds of reading code that all looked correct while a PC session saw
     * nothing, this is the only measurement that can settle it: evt>0 with
     * nothing on the PC means the loss is in transport; evt==0 means the
     * playback path never got here at all (step table empty / state not
     * MC_RUNNING / mc_arm_step never reached). */
    pc_evt_seen++;
    switch (event) {
    case MODE_C_EVT_HIT:
        pc_tx("EVT:HIT,%u,%u", key, step);
        break;
    case MODE_C_EVT_MISS:
        pc_tx("EVT:MISS,%u,%u", key, step);
        break;
    case MODE_C_EVT_DONE:
        pc_tx("EVT:DONE,%u", step);
        break;
    case MODE_C_EVT_STEP:
        pc_tx("EVT:STEP,%u,%u", key, step);
        break;
    case MODE_C_EVT_HID_FAIL:
        pc_tx("EVT:HIDFAIL,%u,%u", key, step);
        break;
    default:
        break;
    }
}

#else

void cdc_emit_event(uint8_t event, uint8_t key, uint16_t step) {
    ARG_UNUSED(event);
    ARG_UNUSED(key);
    ARG_UNUSED(step);
}

#endif

int pc_uart_init(void) {
#if DT_HAS_CHOSEN(zmk_pc_uart) && IS_ENABLED(CONFIG_SERIAL) && IS_ENABLED(CONFIG_USB_CDC_ACM)
    const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zmk_pc_uart));
    if (!device_is_ready(dev)) {
        LOG_WRN("pc-uart not ready; command channel off");
        return -ENODEV;
    }
    k_work_init_delayable(&cmd_work, pc_cmd_work_fn);
    uart_irq_callback_user_data_set(dev, uart_cb, NULL);
    uart_irq_rx_enable(dev);
    LOG_INF("pc-uart command channel ready");
#endif
    return 0;
}
