/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * BAP unicast server: accepts sink (speaker) and source (microphone) ASEs, reports the
 * streaming context of every enable/metadata update and checks the received audio.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/audio/ascs.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/cap.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>

#include "cli.h"
#include "dut.h"

LOG_MODULE_REGISTER(unicast, LOG_LEVEL_INF);

#define SINK_STREAMS 2

struct dut_stream {
	struct bt_cap_stream cap;
	enum bt_audio_dir dir;
	uint16_t ctx;
	struct audio_rx rx;
	uint16_t seq_num;
};

static struct dut_stream streams[SINK_STREAMS + 1]; /* last one is the source */

static const struct bt_bap_qos_cfg_pref qos_pref =
	BT_BAP_QOS_CFG_PREF(true, BT_GAP_LE_PHY_2M, 2U, 20U, 20000U, 40000U, 20000U, 40000U);

NET_BUF_POOL_FIXED_DEFINE(tx_pool, 2, BT_ISO_SDU_BUF_SIZE(CONFIG_BT_ISO_TX_MTU),
			  CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

static struct dut_stream *to_dut(struct bt_bap_stream *bap_stream)
{
	return CONTAINER_OF(bap_stream, struct dut_stream, cap.bap_stream);
}

static const char *dir_str(enum bt_audio_dir dir)
{
	return dir == BT_AUDIO_DIR_SINK ? "sink" : "source";
}

uint8_t unicast_stream_count(void)
{
	uint8_t n = 0;

	for (size_t i = 0; i < ARRAY_SIZE(streams); i++) {
		n += streams[i].cap.bap_stream.ep != NULL;
	}

	return n;
}

static bool find_ctx(struct bt_data *data, void *user_data)
{
	if (data->type == BT_AUDIO_METADATA_TYPE_STREAM_CONTEXT && data->data_len == 2) {
		*(uint16_t *)user_data = sys_get_le16(data->data);
		return false;
	}

	return true;
}

static int parse_ctx(const uint8_t meta[], size_t meta_len, uint16_t *ctx)
{
	*ctx = 0;
	(void)bt_audio_data_parse(meta, meta_len, find_ctx, ctx);

	return *ctx != 0 ? 0 : -ENOENT;
}

/* --- ASCS server callbacks --- */

static int config_cb(struct bt_conn *conn, const struct bt_bap_ep *ep, enum bt_audio_dir dir,
		     const struct bt_audio_codec_cfg *codec_cfg, struct bt_bap_stream **bap_stream,
		     struct bt_bap_qos_cfg_pref *const pref, struct bt_bap_ascs_rsp *rsp)
{
	const size_t first = dir == BT_AUDIO_DIR_SINK ? 0 : SINK_STREAMS;
	const size_t last = dir == BT_AUDIO_DIR_SINK ? SINK_STREAMS : ARRAY_SIZE(streams);

	ARG_UNUSED(conn);
	ARG_UNUSED(ep);
	ARG_UNUSED(codec_cfg);

	for (size_t i = first; i < last; i++) {
		if (streams[i].cap.bap_stream.ep == NULL) {
			streams[i].dir = dir;
			*bap_stream = &streams[i].cap.bap_stream;
			*pref = qos_pref;
			return 0;
		}
	}

	*rsp = BT_BAP_ASCS_RSP(BT_BAP_ASCS_RSP_CODE_NO_MEM, BT_BAP_ASCS_REASON_NONE);

	return -ENOMEM;
}

static int reconfig_cb(struct bt_bap_stream *bap_stream, enum bt_audio_dir dir,
		       const struct bt_audio_codec_cfg *codec_cfg,
		       struct bt_bap_qos_cfg_pref *const pref, struct bt_bap_ascs_rsp *rsp)
{
	ARG_UNUSED(bap_stream);
	ARG_UNUSED(dir);
	ARG_UNUSED(codec_cfg);
	ARG_UNUSED(rsp);

	*pref = qos_pref;

	return 0;
}

static int qos_cb(struct bt_bap_stream *bap_stream, const struct bt_bap_qos_cfg *qos,
		  struct bt_bap_ascs_rsp *rsp)
{
	ARG_UNUSED(bap_stream);
	ARG_UNUSED(qos);
	ARG_UNUSED(rsp);

	return 0;
}

static int check_meta(struct dut_stream *s, const uint8_t meta[], size_t meta_len,
		      struct bt_bap_ascs_rsp *rsp, const char *state)
{
	uint16_t ctx;

	if (parse_ctx(meta, meta_len, &ctx) != 0) {
		*rsp = BT_BAP_ASCS_RSP(BT_BAP_ASCS_RSP_CODE_METADATA_REJECTED,
				       BT_BAP_ASCS_REASON_NONE);
		cli_evt("dut_stream_rejected", "dir=%s reason=no_context", dir_str(s->dir));
		return -EINVAL;
	}

	s->ctx = ctx;
	cli_evt("dut_stream", "state=%s dir=%s ctx=%s", state, dir_str(s->dir), ctx_str(ctx));

	return 0;
}

static int enable_cb(struct bt_bap_stream *bap_stream, const uint8_t meta[], size_t meta_len,
		     struct bt_bap_ascs_rsp *rsp)
{
	return check_meta(to_dut(bap_stream), meta, meta_len, rsp, "enabled");
}

static int start_cb(struct bt_bap_stream *bap_stream, struct bt_bap_ascs_rsp *rsp)
{
	ARG_UNUSED(bap_stream);
	ARG_UNUSED(rsp);

	return 0;
}

static int metadata_cb(struct bt_bap_stream *bap_stream, const uint8_t meta[], size_t meta_len,
		       struct bt_bap_ascs_rsp *rsp)
{
	return check_meta(to_dut(bap_stream), meta, meta_len, rsp, "metadata");
}

static int disable_cb(struct bt_bap_stream *bap_stream, struct bt_bap_ascs_rsp *rsp)
{
	ARG_UNUSED(bap_stream);
	ARG_UNUSED(rsp);

	return 0;
}

static int stop_cb(struct bt_bap_stream *bap_stream, struct bt_bap_ascs_rsp *rsp)
{
	ARG_UNUSED(bap_stream);
	ARG_UNUSED(rsp);

	return 0;
}

static int release_cb(struct bt_bap_stream *bap_stream, struct bt_bap_ascs_rsp *rsp)
{
	ARG_UNUSED(bap_stream);
	ARG_UNUSED(rsp);

	return 0;
}

static const struct bt_bap_unicast_server_cb server_cb = {
	.config = config_cb,
	.reconfig = reconfig_cb,
	.qos = qos_cb,
	.enable = enable_cb,
	.start = start_cb,
	.metadata = metadata_cb,
	.disable = disable_cb,
	.stop = stop_cb,
	.release = release_cb,
};

/* --- Stream callbacks --- */

static void stream_enabled(struct bt_bap_stream *bap_stream)
{
	struct dut_stream *s = to_dut(bap_stream);
	int err;

	/* As the audio sink we are ready to receive right away */
	if (s->dir == BT_AUDIO_DIR_SINK) {
		err = bt_bap_stream_start(bap_stream);
		if (err != 0) {
			LOG_ERR("Receiver start ready failed: %d", err);
		}
	}
}

static void stream_started(struct bt_bap_stream *bap_stream)
{
	struct dut_stream *s = to_dut(bap_stream);
	const struct bt_audio_codec_cfg *cfg = bap_stream->codec_cfg;
	int freq = bt_audio_codec_cfg_get_freq(cfg);
	int octets = bt_audio_codec_cfg_get_octets_per_frame(cfg);
	int dur = bt_audio_codec_cfg_get_frame_dur(cfg);

	s->seq_num = 0;
	if (s->dir == BT_AUDIO_DIR_SINK) {
		(void)audio_rx_start(&s->rx, cfg, "unicast");
	}

	cli_evt("dut_stream", "state=started dir=%s ctx=%s sample_rate=%u frame_us=%u octets=%d",
		dir_str(s->dir), ctx_str(s->ctx),
		freq >= 0 ? bt_audio_codec_cfg_freq_to_freq_hz(freq) : 0,
		dur >= 0 ? bt_audio_codec_cfg_frame_dur_to_frame_dur_us(dur) : 0, octets);
}

static void stream_stopped(struct bt_bap_stream *bap_stream, uint8_t reason)
{
	struct dut_stream *s = to_dut(bap_stream);

	audio_rx_stop(&s->rx);
	cli_evt("dut_stream", "state=stopped dir=%s reason=0x%02x", dir_str(s->dir), reason);
}

static void stream_released(struct bt_bap_stream *bap_stream)
{
	struct dut_stream *s = to_dut(bap_stream);

	audio_rx_stop(&s->rx);
	s->ctx = 0;
	cli_evt("dut_stream", "state=released dir=%s", dir_str(s->dir));
}

static void stream_recv(struct bt_bap_stream *bap_stream, const struct bt_iso_recv_info *info,
			struct net_buf *buf)
{
	audio_rx_frame(&to_dut(bap_stream)->rx, info, buf);
}

static struct bt_bap_stream_ops stream_ops = {
	.enabled = stream_enabled,
	.started = stream_started,
	.stopped = stream_stopped,
	.released = stream_released,
	.recv = stream_recv,
};

/* Microphone: sends silent SDUs on a streaming source ASE */
static void mic_thread(void *p1, void *p2, void *p3)
{
	struct dut_stream *s = &streams[SINK_STREAMS];

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		struct bt_bap_ep_info info;
		struct net_buf *buf;

		if (s->cap.bap_stream.ep == NULL ||
		    bt_bap_ep_get_info(s->cap.bap_stream.ep, &info) != 0 ||
		    info.state != BT_BAP_EP_STATE_STREAMING) {
			k_sleep(K_MSEC(20));
			continue;
		}

		buf = net_buf_alloc(&tx_pool, K_MSEC(50));
		if (buf == NULL) {
			continue;
		}

		net_buf_reserve(buf, BT_ISO_CHAN_SEND_RESERVE);
		memset(net_buf_add(buf, s->cap.bap_stream.qos->sdu), 0, s->cap.bap_stream.qos->sdu);
		if (bt_cap_stream_send(&s->cap, buf, s->seq_num) == 0) {
			s->seq_num++;
		} else {
			net_buf_unref(buf);
			k_sleep(K_MSEC(10));
		}
	}
}

K_THREAD_DEFINE(mic_tid, 2048, mic_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(6), 0, 0);

int unicast_init(void)
{
	const struct bt_bap_unicast_server_register_param param = {
		CONFIG_BT_ASCS_MAX_ASE_SNK_COUNT,
		CONFIG_BT_ASCS_MAX_ASE_SRC_COUNT,
	};
	int err;

	err = bt_bap_unicast_server_register(&param);
	err = err ?: bt_bap_unicast_server_register_cb(&server_cb);
	if (err != 0) {
		return err;
	}

	for (size_t i = 0; i < ARRAY_SIZE(streams); i++) {
		bt_cap_stream_ops_register(&streams[i].cap, &stream_ops);
	}

	return 0;
}
