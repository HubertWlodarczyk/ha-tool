/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Tones on ISO streams (unicast sink ASEs and broadcast BISes) from the precomputed LC3 frame
 * table (lc3_frames.c): no encoder, no DSP.
 */

#ifndef HA_TOOL_AUDIO_TX_H_
#define HA_TOOL_AUDIO_TX_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/cap.h>

enum tone_pattern {
	TONE_CONTINUOUS,
	TONE_RING, /* 1 s on, 1 s off */
	TONE_BEEP, /* 100 ms on, 100 ms off */
};

struct tone {
	uint32_t freq_hz; /* requested; the nearest tone of the table is sent, 0: silence */
	int8_t level_db;  /* dBFS, LC3_TAB_LEVEL_MIN..0 */
	enum tone_pattern pattern;
};

struct lc3_tab_cfg;

/* LC3 configuration of the frame table, NULL if there is none (CLI: audio_cfg_names()) */
const struct lc3_tab_cfg *audio_cfg_find(uint32_t sr_hz, uint32_t frame_us, uint16_t octets);
const char *audio_cfg_names(void);
/* Nearest tone of `cfg`: index > 0 (0 for silence) and its frequency in mHz. -ERANGE if out of
 * the table's range, -EINVAL if `exact` and the tone is not exactly freq_hz.
 */
int audio_tone_resolve(const struct lc3_tab_cfg *cfg, uint32_t freq_hz, bool exact,
		       uint32_t *actual_mhz);
void audio_tone_range(const struct lc3_tab_cfg *cfg, uint32_t *min_hz, uint32_t *max_hz,
		      uint32_t *step_mhz);
/* Level actually sent for level_db (global gain steps of 0.71 dB), in 1/100 dB */
int audio_level_actual_cdb(int8_t level_db);

int audio_tx_init(void);

/* Starts sending `tone` on a streaming CAP stream; -ENOTSUP if its codec configuration is not in
 * the frame table
 */
int audio_tx_start(struct bt_cap_stream *stream, const struct tone *tone);
/* SDUs kept queued in the controller for this stream (BIGs with BN > 1 need BN: the BN SDUs of
 * an event leave together); 0: the default policy
 */
void audio_tx_set_depth(struct bt_cap_stream *stream, uint8_t depth);
void audio_tx_stop(struct bt_cap_stream *stream);
void audio_tx_set_tone(struct bt_cap_stream *stream, const struct tone *tone);
/* To be called from the stream's `sent` callback */
void audio_tx_sent(struct bt_cap_stream *stream);
uint32_t audio_tx_sent_count(struct bt_cap_stream *stream);

/* Short name of an audio context ("media", "ringtone", ...) */
const char *audio_ctx_str(enum bt_audio_context ctx);

#endif /* HA_TOOL_AUDIO_TX_H_ */
