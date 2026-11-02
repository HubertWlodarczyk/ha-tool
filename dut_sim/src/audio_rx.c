/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Receive-side audio check: LC3-decodes every frame and estimates the tone frequency from the
 * spacing of zero crossings (with hysteresis) inside each non-silent frame, so tone patterns
 * (ringtone, beeps) and their on/off edges do not bias the result. Reported once a second as:
 *   EVT dut_audio src=<label> freq=<Hz> level=<dBFS> active=<%> frames=<n> lost=<n>
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/logging/log.h>

#include "cli.h"
#include "dut.h"

LOG_MODULE_REGISTER(audio_rx, LOG_LEVEL_INF);

#define MAX_SAMPLES 480
#define HYSTERESIS  60
#define ACTIVE_PEAK 150 /* -47 dBFS: a frame with a lower peak counts as silence */
#define MAX_GAP     100 /* larger sequence number jumps are a stream restart, not loss */

int audio_rx_start(struct audio_rx *rx, const struct bt_audio_codec_cfg *cfg, const char *label)
{
	int freq = bt_audio_codec_cfg_get_freq(cfg);
	int dur = bt_audio_codec_cfg_get_frame_dur(cfg);
	int octets = bt_audio_codec_cfg_get_octets_per_frame(cfg);

	if (freq < 0 || dur < 0 || octets <= 0) {
		return -EINVAL;
	}

	memset(rx, 0, sizeof(*rx));
	snprintf(rx->label, sizeof(rx->label), "%s", label);
	rx->sr_hz = bt_audio_codec_cfg_freq_to_freq_hz(freq);
	rx->octets = octets;
	/* 44.1 kHz is coded like 48 kHz (same frame of 360 / 480 samples, lasting 8163 /
	 * 10884 us); tone frequency and time base use the real rate.
	 */
	rx->coded_hz = rx->sr_hz == 44100U ? 48000U : rx->sr_hz;
	rx->samples =
		lc3_frame_samples(bt_audio_codec_cfg_frame_dur_to_frame_dur_us(dur), rx->coded_hz);
	rx->decoder = lc3_setup_decoder(bt_audio_codec_cfg_frame_dur_to_frame_dur_us(dur),
					rx->coded_hz, 0, &rx->decoder_mem);
	if (rx->decoder == NULL || rx->samples <= 0) {
		return -EINVAL;
	}

	rx->frame_us = (uint32_t)((uint64_t)rx->samples * 1000000U / rx->sr_hz);

	rx->last_seq = -1;
	rx->active = true;
	LOG_INF("RX %s: %u Hz, %u us, %u octets", label, rx->sr_hz, rx->frame_us, rx->octets);

	return 0;
}

void audio_rx_stop(struct audio_rx *rx)
{
	rx->active = false;
}

static void report(struct audio_rx *rx)
{
	/* half periods / (2 * time they span) */
	const uint32_t freq =
		rx->span_samples > 0
			? (uint32_t)((uint64_t)rx->crossings * rx->sr_hz / 2U / rx->span_samples)
			: 0;
	/* Peak level in 1/10 dB */
	const int level = rx->peak > 0 ? (int)lroundf(200.0f * log10f(rx->peak / 32767.0f)) : -990;

	cli_evt("dut_audio", "src=%s freq=%u level=%s%d.%d active=%u frames=%u lost=%u", rx->label,
		freq, level < 0 ? "-" : "", abs(level) / 10, abs(level) % 10,
		rx->active_frames * 100U / rx->frames, rx->frames, rx->lost);

	rx->frames = 0;
	rx->active_frames = 0;
	rx->lost = 0;
	rx->crossings = 0;
	rx->span_samples = 0;
	rx->peak = 0;
}

/* Counts one frame (lost: concealed by the decoder) */
static void count_frame(struct audio_rx *rx, bool lost)
{
	rx->lost += lost;
	rx->frames++;
	rx->total_frames++;
}

/* Reports once a second of audio is complete */
static void report_if_due(struct audio_rx *rx)
{
	if (rx->frames * rx->frame_us >= USEC_PER_SEC) {
		report(rx);
	}
}

/* Frames the controller never delivered appear only as sequence number gaps: conceal them (PLC)
 * so the time base stays right, and count them as lost.
 */
static void conceal_gap(struct audio_rx *rx, uint16_t seq_num, int16_t *pcm)
{
	uint16_t gap;

	if (rx->last_seq < 0) {
		return;
	}

	gap = (uint16_t)(seq_num - (uint16_t)rx->last_seq - 1U);
	if (gap >= MAX_GAP) {
		return; /* restart of the stream rather than loss */
	}

	for (uint16_t i = 0; i < gap; i++) {
		(void)lc3_decode(rx->decoder, NULL, rx->octets, LC3_PCM_FORMAT_S16, pcm, 1);
		count_frame(rx, true);
		report_if_due(rx);
	}
}

/* Peak level and zero crossings (with hysteresis) of one decoded frame */
static void analyze(struct audio_rx *rx, const int16_t *pcm)
{
	int16_t frame_peak = 0;
	uint32_t frame_crossings = 0;
	int first = -1;
	int last = -1;

	for (int i = 0; i < rx->samples; i++) {
		const int16_t s = pcm[i];
		const int16_t mag = s < 0 ? (s == INT16_MIN ? INT16_MAX : -s) : s;

		frame_peak = MAX(frame_peak, mag);
		if ((s > HYSTERESIS && rx->sign < 0) || (s < -HYSTERESIS && rx->sign > 0)) {
			frame_crossings++;
			first = first < 0 ? i : first;
			last = i;
		}

		if (s > HYSTERESIS) {
			rx->sign = 1;
		} else if (s < -HYSTERESIS) {
			rx->sign = -1;
		}
	}

	rx->peak = MAX(rx->peak, frame_peak);

	/* Only frames with a tone count, so ringtone / beep pauses do not bias the frequency */
	if (frame_peak >= ACTIVE_PEAK) {
		rx->active_frames++;
		if (frame_crossings >= 2) {
			rx->crossings += frame_crossings - 1;
			rx->span_samples += last - first;
		}
	}
}

void audio_rx_frame(struct audio_rx *rx, const struct bt_iso_recv_info *info,
		    const struct net_buf *buf)
{
	static int16_t pcm[MAX_SAMPLES];
	const bool valid = (info->flags & BT_ISO_FLAGS_VALID) != 0 && buf->len >= rx->octets;
	int err;

	if (!rx->active) {
		return;
	}

	conceal_gap(rx, info->seq_num, pcm);
	rx->last_seq = info->seq_num;

	/* An invalid frame is concealed by the decoder (NULL) */
	err = lc3_decode(rx->decoder, valid ? buf->data : NULL, rx->octets, LC3_PCM_FORMAT_S16, pcm,
			 1);
	if (err < 0) {
		LOG_WRN("decode failed: %d", err);
	}

	count_frame(rx, !valid);
	analyze(rx, pcm);
	report_if_due(rx);
}

const char *ctx_str(uint16_t ctx)
{
	switch (ctx) {
	case BT_AUDIO_CONTEXT_TYPE_UNSPECIFIED:
		return "unspecified";
	case BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL:
		return "conversational";
	case BT_AUDIO_CONTEXT_TYPE_MEDIA:
		return "media";
	case BT_AUDIO_CONTEXT_TYPE_RINGTONE:
		return "ringtone";
	case BT_AUDIO_CONTEXT_TYPE_ALERTS:
		return "alerts";
	case BT_AUDIO_CONTEXT_TYPE_NOTIFICATIONS:
		return "notifications";
	case BT_AUDIO_CONTEXT_TYPE_LIVE:
		return "live";
	case 0:
		return "none";
	default:
		return "other";
	}
}
