/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Feeds sine tones to ISO streams (unicast sink ASEs and broadcast BISes) from the precomputed
 * LC3 frame table (lc3_frames.c): no encoder, no DSP. A tone is a short cycle of frames sent in
 * a loop; its level is set by rewriting the global gain field of each frame. Each stream keeps
 * at most TX_INFLIGHT (1 with many streams) SDUs queued in the controller; the `sent` callback
 * releases a slot.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/cap.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include "audio_tx.h"
#include "lc3_frames.h"

LOG_MODULE_REGISTER(audio_tx, LOG_LEVEL_INF);

/* Every broadcast BIS plus the unicast sink stream */
#define TX_SLOTS      (CONFIG_HA_BCAST_MAX_BIS_TOTAL + 1)
#define TX_INFLIGHT   2
/* The TX thread also wakes up on its own, to retry after a full buffer pool */
#define TX_POLL       K_MSEC(5)
/* ISO TX buffers of the controller (BT_CTLR_ISO_TX_BUFFERS = host BT_ISO_TX_BUF_COUNT): with
 * more streams than half of them, 2 SDUs per stream in flight would starve some streams, so
 * each stream gets 1 (the next SDU follows its `sent` callback, well within the interval).
 */
#define CTLR_ISO_BUFS CONFIG_BT_ISO_TX_BUF_COUNT

#define TX_BUFS (TX_SLOTS * TX_INFLIGHT)

/* Every frame of the table fits into one ISO SDU */
BUILD_ASSERT(LC3_TAB_MAX_OCTETS <= CONFIG_BT_ISO_TX_MTU, "raise CONFIG_BT_ISO_TX_MTU");

NET_BUF_POOL_FIXED_DEFINE(tx_pool, TX_BUFS, BT_ISO_SDU_BUF_SIZE(CONFIG_BT_ISO_TX_MTU),
			  CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

struct tx_slot {
	struct bt_cap_stream *stream;
	struct tone tone;
	const struct lc3_tab_cfg *cfg;
	const uint8_t *frames; /* cycle of the tone, NULL: silence */
	uint8_t nframes;
	int16_t gain_delta; /* added to the global gain of every frame */
	uint16_t seq_num;
	uint32_t frame_count;
	uint32_t sent_count;
	int inflight;
};

/*
 * slots_lock (mutex): held by the TX thread while sending and by start/stop/set_tone.
 * bind_lock (spinlock): protects stream, inflight and sent_count, which are also touched from the
 * Bluetooth `sent` callback; that callback must never wait for the mutex.
 */
static struct tx_slot slots[TX_SLOTS];
static K_MUTEX_DEFINE(slots_lock);
static struct k_spinlock bind_lock;
static K_SEM_DEFINE(tx_sem, 0, 1);

/* --- Frame table --- */

const struct lc3_tab_cfg *audio_cfg_find(uint32_t sr_hz, uint32_t frame_us, uint16_t octets)
{
	for (size_t i = 0; i < ARRAY_SIZE(lc3_tab); i++) {
		if (lc3_tab[i].sr_hz == sr_hz && lc3_tab[i].frame_us == frame_us &&
		    lc3_tab[i].octets == octets) {
			return &lc3_tab[i];
		}
	}

	return NULL;
}

/* Tone m has the frequency m * sr / (cycle * samples) */
static uint32_t tone_mhz(const struct lc3_tab_cfg *cfg, uint32_t m)
{
	const uint64_t div = (uint64_t)cfg->cycle * cfg->samples;

	return (uint32_t)(((uint64_t)m * cfg->sr_hz * 1000U + div / 2U) / div);
}

int audio_tone_resolve(const struct lc3_tab_cfg *cfg, uint32_t freq_hz, bool exact,
		       uint32_t *actual_mhz)
{
	const uint32_t div = cfg->cycle * cfg->samples;
	uint32_t m;

	if (freq_hz == 0U) {
		*actual_mhz = 0;
		return 0;
	}

	m = (uint32_t)(((uint64_t)freq_hz * div + cfg->sr_hz / 2U) / cfg->sr_hz);
	if (m < cfg->m_min || m > cfg->m_max) {
		return -ERANGE;
	}

	*actual_mhz = tone_mhz(cfg, m);
	if (exact && *actual_mhz != freq_hz * 1000U) {
		return -EINVAL;
	}

	return (int)m;
}

void audio_tone_range(const struct lc3_tab_cfg *cfg, uint32_t *min_hz, uint32_t *max_hz,
		      uint32_t *step_mhz)
{
	*min_hz = (tone_mhz(cfg, cfg->m_min) + 999U) / 1000U;
	*max_hz = tone_mhz(cfg, cfg->m_max) / 1000U;
	*step_mhz = tone_mhz(cfg, 1);
}

/* Global gain steps of 20/28 dB: level_db * 1.4, rounded */
static int16_t level_to_delta(int8_t level_db)
{
	const int x = level_db * 14;

	return (int16_t)(x >= 0 ? (x + 5) / 10 : (x - 5) / 10);
}

int audio_level_actual_cdb(int8_t level_db)
{
	const int x = level_to_delta(level_db) * 2000;

	return x >= 0 ? (x + 14) / 28 : (x - 14) / 28;
}

const char *audio_cfg_names(void)
{
	return "8_1 8_2 16_1 16_2 24_1 24_2 32_1 32_2 441_1 441_2 48_1..48_6";
}

/* Writes the 8 bit global gain at `bit` (LSB first, counted from the end of the frame) */
static void patch_gain(uint8_t *frame, uint16_t len, uint8_t bit, int16_t delta)
{
	unsigned int v = 0;
	int g;

	for (int i = 0; i < 8; i++) {
		const unsigned int b = bit + i;

		v |= ((frame[len - 1U - b / 8U] >> (b % 8U)) & 1U) << i;
	}

	g = CLAMP((int)v + delta, 0, 255);

	for (int i = 0; i < 8; i++) {
		const unsigned int b = bit + i;
		uint8_t *p = &frame[len - 1U - b / 8U];

		*p = (*p & ~BIT(b % 8U)) | (((g >> i) & 1) << (b % 8U));
	}
}

/* Selects the frames of slot->tone; slots_lock held */
static void slot_load(struct tx_slot *slot)
{
	uint32_t actual, min_hz, max_hz, step;
	int m;

	/* Validated by the CLI for broadcasts; a unicast configuration is only known after the
	 * negotiation, so its tone is capped to the table's range.
	 */
	audio_tone_range(slot->cfg, &min_hz, &max_hz, &step);
	m = audio_tone_resolve(slot->cfg,
			       slot->tone.freq_hz == 0U ? 0U
							: CLAMP(slot->tone.freq_hz, min_hz, max_hz),
			       false, &actual);

	slot->frames = NULL;
	slot->nframes = 1;
	slot->frame_count = 0;
	slot->gain_delta = level_to_delta(slot->tone.level_db);

	if (m > 0) {
		const uint32_t i = m - slot->cfg->m_min;
		const uint32_t len = slot->cfg->offs[i + 1] - slot->cfg->offs[i];

		slot->frames = &slot->cfg->blob[slot->cfg->offs[i]];
		slot->nframes = len / slot->cfg->octets;
	}
}

static struct tx_slot *find_slot(const struct bt_cap_stream *stream)
{
	for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
		if (slots[i].stream == stream) {
			return &slots[i];
		}
	}

	return NULL;
}

static bool tone_on(const struct tx_slot *slot)
{
	/* 64 bit: frame_count * frame_us passes 2^32 after 71 minutes of 10 ms frames */
	const uint32_t t_ms =
		(uint32_t)(((uint64_t)slot->frame_count * slot->cfg->frame_us) / 1000U);

	switch (slot->tone.pattern) {
	case TONE_RING:
		return (t_ms % 2000U) < 1000U;
	case TONE_BEEP:
		return (t_ms % 200U) < 100U;
	case TONE_CONTINUOUS:
	default:
		return true;
	}
}

static void send_one(struct tx_slot *slot)
{
	const uint16_t octets = slot->cfg->octets;
	struct net_buf *buf;
	uint8_t *out;
	int err;

	buf = net_buf_alloc(&tx_pool, K_NO_WAIT);
	if (buf == NULL) {
		return;
	}

	net_buf_reserve(buf, BT_ISO_CHAN_SEND_RESERVE);
	out = net_buf_add(buf, octets);

	if (slot->frames != NULL && tone_on(slot)) {
		memcpy(out, slot->frames + (slot->frame_count % slot->nframes) * octets, octets);
		if (slot->gain_delta != 0) {
			patch_gain(out, octets, slot->cfg->gidx_bit, slot->gain_delta);
		}
	} else {
		memcpy(out, slot->cfg->silence, octets);
	}

	K_SPINLOCK(&bind_lock) {
		slot->inflight++;
	}

	err = bt_cap_stream_send(slot->stream, buf, slot->seq_num);
	if (err != 0) {
		LOG_DBG("send failed: %d", err);
		K_SPINLOCK(&bind_lock) {
			slot->inflight--;
		}
		net_buf_unref(buf);
		return;
	}

	/* Only a frame that was queued advances the tone: after a failed send the same frame goes
	 * next, so the tone keeps its phase (skipping one frame of a 2-frame cycle inverts it).
	 */
	slot->frame_count++;
	slot->seq_num++;
}

static int inflight_max(void)
{
	int active = 0;

	K_SPINLOCK(&bind_lock) {
		for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
			active += slots[i].stream != NULL;
		}
	}

	return active * TX_INFLIGHT <= CTLR_ISO_BUFS ? TX_INFLIGHT : 1;
}

/* SDUs of the slot in flight; "full" for an unbound slot */
static int inflight(const struct tx_slot *slot)
{
	int n = 0;

	K_SPINLOCK(&bind_lock) {
		n = slot->stream != NULL ? slot->inflight : TX_INFLIGHT;
	}

	return n;
}

static void tx_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		k_mutex_lock(&slots_lock, K_FOREVER);
		const int max = inflight_max();

		for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
			struct tx_slot *slot = &slots[i];

			while (inflight(slot) < max) {
				const int before = inflight(slot);

				send_one(slot);
				if (inflight(slot) <= before) {
					break; /* no buffer or send error, retry later */
				}
			}
		}
		k_mutex_unlock(&slots_lock);

		(void)k_sem_take(&tx_sem, TX_POLL);
	}
}

K_THREAD_STACK_DEFINE(tx_stack, 1024);
static struct k_thread tx_thread_data;

int audio_tx_init(void)
{
	k_thread_create(&tx_thread_data, tx_stack, K_THREAD_STACK_SIZEOF(tx_stack), tx_thread, NULL,
			NULL, NULL, K_PRIO_PREEMPT(4), 0, K_NO_WAIT);
	k_thread_name_set(&tx_thread_data, "audio_tx");

	return 0;
}

int audio_tx_start(struct bt_cap_stream *stream, const struct tone *tone)
{
	const struct bt_audio_codec_cfg *codec = stream->bap_stream.codec_cfg;
	const struct lc3_tab_cfg *cfg;
	struct tx_slot *slot;
	int freq, dur, octets;

	if (codec == NULL) {
		return -EINVAL;
	}

	freq = bt_audio_codec_cfg_get_freq(codec);
	dur = bt_audio_codec_cfg_get_frame_dur(codec);
	octets = bt_audio_codec_cfg_get_octets_per_frame(codec);
	if (freq < 0 || dur < 0 || octets <= 0) {
		return -EINVAL;
	}

	cfg = audio_cfg_find(bt_audio_codec_cfg_freq_to_freq_hz(freq),
			     bt_audio_codec_cfg_frame_dur_to_frame_dur_us(dur), octets);
	if (cfg == NULL) {
		LOG_WRN("No frames for %d Hz / %d us / %d octets",
			bt_audio_codec_cfg_freq_to_freq_hz(freq),
			bt_audio_codec_cfg_frame_dur_to_frame_dur_us(dur), octets);
		return -ENOTSUP;
	}

	k_mutex_lock(&slots_lock, K_FOREVER);

	slot = find_slot(stream);
	if (slot == NULL) {
		slot = find_slot(NULL);
	}

	if (slot == NULL) {
		k_mutex_unlock(&slots_lock);
		return -ENOMEM;
	}

	K_SPINLOCK(&bind_lock) {
		slot->stream = NULL;
	}

	memset(slot, 0, sizeof(*slot));
	slot->cfg = cfg;
	slot->tone = *tone;
	slot_load(slot);

	K_SPINLOCK(&bind_lock) {
		slot->inflight = 0;
		slot->stream = stream;
	}
	k_mutex_unlock(&slots_lock);

	LOG_INF("TX start %p: %s, tone %u Hz (%u frames)", (void *)stream, cfg->name, tone->freq_hz,
		slot->nframes);
	k_sem_give(&tx_sem);

	return 0;
}

void audio_tx_stop(struct bt_cap_stream *stream)
{
	struct tx_slot *slot;

	k_mutex_lock(&slots_lock, K_FOREVER);
	slot = find_slot(stream);
	if (slot != NULL) {
		K_SPINLOCK(&bind_lock) {
			slot->stream = NULL;
		}
	}
	k_mutex_unlock(&slots_lock);
}

void audio_tx_set_tone(struct bt_cap_stream *stream, const struct tone *tone)
{
	struct tx_slot *slot;

	k_mutex_lock(&slots_lock, K_FOREVER);
	slot = find_slot(stream);
	if (slot != NULL) {
		slot->tone = *tone;
		slot_load(slot);
	}
	k_mutex_unlock(&slots_lock);
}

void audio_tx_sent(struct bt_cap_stream *stream)
{
	/* Bluetooth TX path: only the spinlock, the TX thread may hold the mutex */
	K_SPINLOCK(&bind_lock) {
		for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
			if (slots[i].stream == stream) {
				if (slots[i].inflight > 0) {
					slots[i].inflight--;
				}
				slots[i].sent_count++;
				break;
			}
		}
	}

	k_sem_give(&tx_sem);
}

uint32_t audio_tx_sent_count(struct bt_cap_stream *stream)
{
	uint32_t count = 0;

	K_SPINLOCK(&bind_lock) {
		struct tx_slot *slot = find_slot(stream);

		count = slot != NULL ? slot->sent_count : 0;
	}

	return count;
}

const char *audio_ctx_str(enum bt_audio_context ctx)
{
	switch (ctx) {
	case BT_AUDIO_CONTEXT_TYPE_UNSPECIFIED:
		return "unspecified";
	case BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL:
		return "conversational";
	case BT_AUDIO_CONTEXT_TYPE_MEDIA:
		return "media";
	case BT_AUDIO_CONTEXT_TYPE_GAME:
		return "game";
	case BT_AUDIO_CONTEXT_TYPE_INSTRUCTIONAL:
		return "instructional";
	case BT_AUDIO_CONTEXT_TYPE_VOICE_ASSISTANTS:
		return "voice_assistants";
	case BT_AUDIO_CONTEXT_TYPE_LIVE:
		return "live";
	case BT_AUDIO_CONTEXT_TYPE_SOUND_EFFECTS:
		return "sound_effects";
	case BT_AUDIO_CONTEXT_TYPE_NOTIFICATIONS:
		return "notifications";
	case BT_AUDIO_CONTEXT_TYPE_RINGTONE:
		return "ringtone";
	case BT_AUDIO_CONTEXT_TYPE_ALERTS:
		return "alerts";
	case BT_AUDIO_CONTEXT_TYPE_EMERGENCY_ALARM:
		return "emergency_alarm";
	default:
		return "mixed";
	}
}
