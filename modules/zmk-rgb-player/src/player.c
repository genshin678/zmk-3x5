/*
 * player.c - score playback engine
 *
 * Score binary format (little-endian):
 *   [0..1]  u16  note count N
 *   [2..]   N * 8-byte record:
 *           u32  startMs
 *           u16  keyIndex (1..15)
 *           u16  durationMs
 *
 * Playback runs on a dedicated k_work_delayable scheduled every 5 ms.
 * Each tick:
 *   - compute current position_ms
 *   - fire every note whose startMs <= position_ms (advance head index)
 *   - on fire: light LED for keyIndex-1, and (mode B) call the &kp behavior
 *     for the corresponding scancode to emit a HID keypress.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/hid.h>
#include <zmk/endpoints.h>
#include <dt-bindings/zmk/keys.h>
#include <zmk_rgbeffect/player.h>
#include <zmk_rgbeffect/effects.h>

LOG_MODULE_DECLARE(zmk_rgbeffect, CONFIG_ZMK_RGB_PLAYER_LOG_LEVEL);

/* Per-key scancode table. Indexed by keyIndex 0..14. */
static const uint8_t KEY_SCANCODE[15] = {
    0x1C, /* Y */
    0x18, /* U */
    0x0C, /* I */
    0x12, /* O */
    0x13, /* P */
    0x0B, /* H */
    0x0D, /* J */
    0x0E, /* K */
    0x0F, /* L */
    0x33, /* ; */
    0x11, /* N */
    0x10, /* M */
    0x36, /* , */
    0x37, /* . */
    0x38, /* / */
};

typedef struct __attribute__((packed)) {
    uint32_t start_ms;
    uint16_t key_index;   /* 1..15 */
    uint16_t duration_ms;
} note_t;

static uint8_t  score_buf[PLAYER_BUF_BYTES];
static uint16_t score_len = 0;
static note_t  *notes = NULL;
static uint16_t note_count = 0;
static uint16_t next_idx = 0;

static player_mode_t  mode = PLAYER_MODE_A;
static player_state_t state = PLAYER_STOPPED;
static int64_t play_start_tick = 0;
static int64_t paused_offset_ms = 0;

static struct k_work_delayable play_work;
static atomic_t play_tick_running;

static void fire_note(uint8_t key_index_0_14, uint16_t duration_ms);
static void schedule_next_tick(void);

/* Non-blocking key release for Mode B: each key gets its own delayed work so
 * we never block the system workqueue (which also drives the LED effects).
 * Without this, k_msleep here would freeze the ripple/LED tick during play. */
struct player_rel_item {
    struct k_work_delayable dwork;
    uint8_t idx;
};
static struct player_rel_item rel_items[LED_PIXEL_COUNT];

static void player_rel_fn(struct k_work *work) {
    struct player_rel_item *it =
        CONTAINER_OF(work, struct player_rel_item, dwork.work);
    zmk_hid_keyboard_release(KEY_SCANCODE[it->idx]);
    zmk_endpoints_send_report(0x07); /* HID keyboard usage page */
    effects_on_key_up(it->idx);
}

static void play_tick(struct k_work *work) {
    if (state != PLAYER_PLAYING) return;

    int64_t now = k_uptime_get();
    uint32_t position_ms = (uint32_t)(now - play_start_tick + paused_offset_ms);

    /* Fire all notes whose start has been reached. */
    while (next_idx < note_count && notes[next_idx].start_ms <= position_ms) {
        fire_note((uint8_t)(notes[next_idx].key_index - 1),
                  notes[next_idx].duration_ms);
        next_idx++;
    }

    /* If past end, stop. */
    if (next_idx >= note_count) {
        state = PLAYER_STOPPED;
        effects_player_exit();
        atomic_clear(&play_tick_running);
        return;
    }
    schedule_next_tick();
}

static void schedule_next_tick(void) {
    /* Use bit 0 as the "tick work already initialized" flag.
     * Zephyr 3.5 atomic API is per-bit, not whole-word. */
    if (!atomic_test_and_set_bit(&play_tick_running, 0)) {
        k_work_init_delayable(&play_work, play_tick);
    }
    k_work_schedule(&play_work, K_MSEC(5));
}

/* Fire a note: trigger ripple effect at that key and (mode B) tap scancode. */
static void fire_note(uint8_t idx, uint16_t duration_ms) {
    if (idx >= LED_PIXEL_COUNT) return;

    /* Tell the ripple effect where the wave should start from. */
    effects_on_key_down(idx);

    if (mode == PLAYER_MODE_B) {
        /* Press the key for a brief pulse so the game's chord detector
         * registers it but the key doesn't get stuck, then push the HID
         * report out over the active transport (USB/BLE). The release is
         * scheduled (non-blocking) so the system workqueue stays free. */
        uint16_t press_ms = duration_ms < 60 ? duration_ms : 60;
        if (press_ms < 20) press_ms = 20;
        zmk_hid_keyboard_press(KEY_SCANCODE[idx]);
        zmk_endpoints_send_report(0x07); /* HID keyboard usage page */
        rel_items[idx].idx = idx;
        k_work_schedule(&rel_items[idx].dwork, K_MSEC(press_ms));
    }
}

int player_init(void) {
    for (int i = 0; i < LED_PIXEL_COUNT; i++) {
        k_work_init_delayable(&rel_items[i].dwork, player_rel_fn);
    }
    return 0;
}

int player_load(const uint8_t *buf, uint16_t len) {
    if (!buf || len < 2) return -EINVAL;
    if (len > sizeof(score_buf)) return -ENOMEM;
    memcpy(score_buf, buf, len);
    score_len = len;

    note_count = (uint16_t)(score_buf[0] | (score_buf[1] << 8));
    if (note_count > PLAYER_MAX_NOTES) {
        note_count = PLAYER_MAX_NOTES;
    }
    notes = (note_t *)(score_buf + 2);
    next_idx = 0;
    LOG_INF("player: loaded %u notes (%u bytes)", note_count, score_len);
    return 0;
}

void player_clear(void) {
    score_len = 0;
    note_count = 0;
    next_idx = 0;
    notes = NULL;
}

int player_play(void) {
    if (note_count == 0) return -ENOENT;
    state = PLAYER_PLAYING;
    next_idx = 0;
    paused_offset_ms = 0;
    play_start_tick = k_uptime_get();
    effects_player_enter();
    schedule_next_tick();
    return 0;
}

int player_pause(void) {
    if (state != PLAYER_PLAYING) return -EINVAL;
    state = PLAYER_PAUSED;
    paused_offset_ms += k_uptime_delta(&play_start_tick);
    atomic_clear(&play_tick_running);
    k_work_cancel_delayable(&play_work);
    return 0;
}

int player_stop(void) {
    state = PLAYER_STOPPED;
    paused_offset_ms = 0;
    next_idx = 0;
    atomic_clear(&play_tick_running);
    k_work_cancel_delayable(&play_work);
    effects_player_exit();
    return 0;
}

void player_set_mode(player_mode_t m) { mode = m; }
player_mode_t player_get_mode(void) { return mode; }
player_state_t player_get_state(void) { return state; }
uint32_t player_get_position_ms(void) {
    if (state != PLAYER_PLAYING) return 0;
    return (uint32_t)(k_uptime_get() - play_start_tick + paused_offset_ms);
}
