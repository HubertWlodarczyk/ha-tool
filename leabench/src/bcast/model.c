/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/* Broadcast drafts (struct big) and their effective configuration: codec per BIS, QoS. */

#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/crypto.h>

#include "bcast.h"
#include "lc3_frames.h"
#include "bcast/internal.h"

const struct tone default_tone = {.freq_hz = 500, .level_db = -12, .pattern = TONE_CONTINUOUS};

struct big bcast_bigs[MAX_BIGS];

struct big *big_of_stream(const struct bt_bap_stream *bap_stream, int *bis_index)
{
	for (int b = 0; b < MAX_BIGS; b++) {
		for (int i = 0; i < bcast_bigs[b].bis_count; i++) {
			if (&bcast_bigs[b].bis[i].stream.bap_stream == bap_stream) {
				*bis_index = i;
				return &bcast_bigs[b];
			}
		}
	}

	return NULL;
}

int big_number(const struct big *big)
{
	return (int)(big - bcast_bigs) + 1;
}

const char *bcast_state_str(void)
{
	const char *state = "none";

	for (int b = 0; b < MAX_BIGS; b++) {
		if (bcast_bigs[b].state == BIG_STREAMING) {
			return "streaming";
		}

		if (bcast_bigs[b].state == BIG_DRAFT) {
			state = "draft";
		}
	}

	return state;
}

uint32_t sg_octets(const struct subgroup *sg)
{
	const int octets = bt_audio_codec_cfg_get_octets_per_frame(&sg->preset.codec_cfg);

	return octets > 0 ? octets : 0;
}

uint32_t bis_octets(const struct big *big, const struct bis *bis)
{
	return bis->octets != 0 ? bis->octets : sg_octets(&big->sg[bis->sg]);
}

uint32_t sg_frame_us(const struct subgroup *sg)
{
	const int dur = bt_audio_codec_cfg_get_frame_dur(&sg->preset.codec_cfg);

	return dur >= 0 ? bt_audio_codec_cfg_frame_dur_to_frame_dur_us(dur) : 0;
}

uint32_t sg_rate_hz(const struct subgroup *sg)
{
	const int freq = bt_audio_codec_cfg_get_freq(&sg->preset.codec_cfg);

	return freq >= 0 ? bt_audio_codec_cfg_freq_to_freq_hz(freq) : 0;
}

uint32_t bis_rate_hz(const struct big *big, const struct bis *bis)
{
	return bis->rate_hz != 0 ? bis->rate_hz : sg_rate_hz(&big->sg[bis->sg]);
}

/* Frame table entry of a BIS (sampling rate, frame duration, octets), NULL if there is none */
const struct lc3_tab_cfg *bis_cfg(const struct big *big, const struct bis *bis)
{
	return audio_cfg_find(bis_rate_hz(big, bis), sg_frame_us(&big->sg[bis->sg]),
			      bis_octets(big, bis));
}

/* SDU interval of a BIS: the real frame duration (44.1 kHz frames last 8163 / 10884 us) */
uint32_t bis_sdu_interval_us(const struct big *big, const struct bis *bis)
{
	const uint32_t frame_us = sg_frame_us(&big->sg[bis->sg]);

	if (bis_rate_hz(big, bis) == 44100U) {
		return frame_us == 7500U ? 8163U : 10884U;
	}

	return frame_us;
}

/* BIG QoS: first subgroup's preset with the BIG overrides; max SDU from the largest BIS */
void big_effective_qos(const struct big *big, struct bt_bap_qos_cfg *qos)
{
	uint16_t sdu = 0;

	*qos = big->sg[0].preset.qos;
	if (big->ovr.phy >= 0) {
		qos->phy = big->ovr.phy;
	}

	if (big->ovr.interval_us >= 0) {
		qos->interval = big->ovr.interval_us;
	}

	if (big->ovr.rtn >= 0) {
		qos->rtn = big->ovr.rtn;
	}

	if (big->ovr.latency_ms >= 0) {
		qos->latency = big->ovr.latency_ms;
	}

	if (big->ovr.pd_us >= 0) {
		qos->pd = big->ovr.pd_us;
	}

	if (big->ovr.framing >= 0) {
		qos->framing = big->ovr.framing;
	}

	/* iso=: the latency that makes the controller send iso/interval SDUs per event (BN) */
	if (big->ovr.iso_us > 0 && qos->interval > 0) {
		const uint32_t bn = big->ovr.iso_us / qos->interval;

		qos->latency = DIV_ROUND_UP((bn + 1U) * qos->interval, 1000U);
	}

	for (int i = 0; i < big->bis_count; i++) {
		sdu = MAX(sdu, bis_octets(big, &big->bis[i]));
	}

	qos->sdu = sdu;
}

void big_init_draft(struct big *big, int number)
{
	/* Stack objects of an earlier run that were still stopping (big_release keeps them) */
	struct bt_cap_broadcast_source *source = big->source;
	struct bt_le_ext_adv *adv = big->adv;

	memset(big, 0, sizeof(*big));
	big->source = source;
	big->adv = adv;
	big->state = BIG_DRAFT;
	snprintf(big->name, sizeof(big->name), "LEA Bench Auracast %d", number);
	(void)bt_rand(&big->broadcast_id, BT_AUDIO_BROADCAST_ID_SIZE);
	big->broadcast_id &= 0xFFFFFF;
	big->pbp_enabled = true;
	big->packing = BT_ISO_PACKING_SEQUENTIAL;
	big->pa_ms = 60; /* BT_BAP_PER_ADV_PARAM_BROADCAST_FAST */
	big->txpower = TXPOWER_DEFAULT;
	big->ovr = (struct big_qos_override){-1, -1, -1, -1, -1, -1, -1};

	/* The memset cleared the stream callbacks */
	for (int i = 0; i < MAX_BIS; i++) {
		bt_cap_stream_ops_register(&big->bis[i].stream, &bcast_stream_ops);
	}
}
