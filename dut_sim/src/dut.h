/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef DUT_H_
#define DUT_H_

#include <stdbool.h>
#include <stdint.h>

#include <lc3.h>

#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/net_buf.h>

#define DUT_VERSION "0.1.0"

#define DUT_SINK_CONTEXTS                                                                          \
	(BT_AUDIO_CONTEXT_TYPE_UNSPECIFIED | BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL |                \
	 BT_AUDIO_CONTEXT_TYPE_MEDIA | BT_AUDIO_CONTEXT_TYPE_RINGTONE |                            \
	 BT_AUDIO_CONTEXT_TYPE_ALERTS | BT_AUDIO_CONTEXT_TYPE_NOTIFICATIONS |                      \
	 BT_AUDIO_CONTEXT_TYPE_LIVE)
#define DUT_SOURCE_CONTEXTS                                                                        \
	(BT_AUDIO_CONTEXT_TYPE_UNSPECIFIED | BT_AUDIO_CONTEXT_TYPE_CONVERSATIONAL)

/* Decodes received LC3 frames and reports tone frequency and level once per second */
struct audio_rx {
	char label[24];
	lc3_decoder_t decoder;
	uint32_t sr_hz;    /* real sampling rate */
	uint32_t coded_hz; /* rate the LC3 frames are coded at (44.1 kHz: 48 kHz) */
	int samples;       /* per frame */
	uint32_t frame_us; /* real frame duration */
	uint16_t octets;
	bool active;
	uint32_t frames;
	uint32_t active_frames;
	uint32_t lost;
	uint32_t clipped;      /* decoded samples at full scale: the tone was clipped */
	uint32_t crossings;    /* half periods counted inside frames */
	uint32_t span_samples; /* samples spanned by those half periods */
	int8_t sign;
	int16_t peak;
	uint32_t total_frames;
	int32_t last_seq; /* -1: none yet */

	/* Decoder memory for the largest configuration (10 ms, 48 kHz) */
	LC3_DECODER_MEM_T(10000, 48000) decoder_mem;
};

int audio_rx_start(struct audio_rx *rx, const struct bt_audio_codec_cfg *cfg, const char *label);
void audio_rx_stop(struct audio_rx *rx);
void audio_rx_frame(struct audio_rx *rx, const struct bt_iso_recv_info *info,
		    const struct net_buf *buf);

const char *ctx_str(uint16_t ctx);

int unicast_init(void);
uint8_t unicast_stream_count(void);

/* Pairing callbacks, IO test double, bond commands (pairing.c) */
int pairing_init(void);
int dut_bond_count(void);

/* Volume renderer, HAS and MICP servers (ha_services.c) */
int ha_services_init(void);

/* Call and media control clients, the hearing aid buttons (ctrl.c) */
int ctrl_init(void);
void ctrl_on_secure(struct bt_conn *conn);
void ctrl_on_disconnected(struct bt_conn *conn);

/*
 * Pauses connectable advertising while a broadcast's periodic advertising sync is established.
 * The Zephyr controller lets the AUX event of its own advertising preempt the sync's receive
 * window; with an advertising interval that divides the PA interval (30 ms vs. 60 ms, the BAP
 * defaults) that happens at every PA event on an unlucky phase and the sync fails (0x3e).
 */
void dut_adv_hold(bool hold);

int bsink_init(void);
void bsink_reset(void);
const char *bsink_state_str(void);

#endif /* DUT_H_ */
