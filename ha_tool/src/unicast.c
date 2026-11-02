/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * CAP / BAP unicast streams towards the hearing aid on one link: discovery, opening a sink
 * (and for calls a source) stream with a streaming context, switching the context of running
 * streams (metadata update), closing. All procedures block until the peer completed them, so
 * they are only called from the phone worker thread.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/cap.h>
#include <zephyr/bluetooth/audio/csip.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "cli.h"
#include "phone.h"
#include "presets.h"

LOG_MODULE_REGISTER(unicast, LOG_LEVEL_INF);

#define PROC_TIMEOUT  K_SECONDS(10)
#define START_TIMEOUT K_SECONDS(15)

static struct {
	struct bt_conn *conn;
	uint8_t dev_id;
	bool discovered;
	struct bt_bap_ep *snk_ep;
	struct bt_bap_ep *src_ep;

	struct bt_cap_stream snk;
	struct bt_cap_stream src;
	struct bt_cap_unicast_group *group;
	struct bt_bap_lc3_preset preset; /* active codec + QoS config, metadata updated in place */
	struct unicast_cfg cur;
	bool streaming;
	atomic_t rx_count;
	atomic_t rx_lost;
	int32_t rx_last_seq; /* -1: none yet */
} uc;

static K_SEM_DEFINE(sem_proc, 0, 1);
static K_SEM_DEFINE(sem_sec, 0, 1);
static int proc_err;

static void proc_done(int err)
{
	proc_err = err;
	k_sem_give(&sem_proc);
}

static int wait_proc(k_timeout_t timeout)
{
	return k_sem_take(&sem_proc, timeout) != 0 ? -ETIMEDOUT : proc_err;
}

/* --- Link --- */

void unicast_attach(struct bt_conn *conn, uint8_t dev_id)
{
	if (conn == uc.conn) {
		return;
	}

	if (uc.conn != NULL) {
		bt_conn_unref(uc.conn);
	}

	uc.conn = conn != NULL ? bt_conn_ref(conn) : NULL;
	uc.dev_id = dev_id;
	uc.discovered = false;
	uc.snk_ep = NULL;
	uc.src_ep = NULL;
}

struct bt_conn *unicast_conn(void)
{
	return uc.conn;
}

uint8_t unicast_dev_id(void)
{
	return uc.dev_id;
}

bool unicast_discovered(void)
{
	return uc.discovered;
}

bool unicast_has_sink(void)
{
	return uc.snk_ep != NULL;
}

bool unicast_has_source(void)
{
	return uc.src_ep != NULL;
}

bool unicast_streaming(void)
{
	return uc.streaming;
}

const struct unicast_cfg *unicast_current(void)
{
	return uc.streaming ? &uc.cur : NULL;
}

uint32_t unicast_tx_count(void)
{
	return audio_tx_sent_count(&uc.snk);
}

uint32_t unicast_rx_count(void)
{
	return atomic_get(&uc.rx_count);
}

uint32_t unicast_rx_lost(void)
{
	return atomic_get(&uc.rx_lost);
}

void unicast_link_lost(void)
{
	if (uc.group != NULL) {
		(void)bt_cap_unicast_group_delete(uc.group);
		uc.group = NULL;
	}

	uc.streaming = false;
	unicast_attach(NULL, uc.dev_id);
}

int unicast_ensure_profile(void)
{
	struct bt_conn_info info;
	int err;

	if (uc.conn == NULL) {
		return -ENOTCONN;
	}

	if (uc.discovered) {
		return 0;
	}

	err = bt_conn_get_info(uc.conn, &info);
	if (err != 0) {
		return err;
	}

	if (info.security.level < BT_SECURITY_L2) {
		k_sem_reset(&sem_sec);
		err = bt_conn_set_security(uc.conn, BT_SECURITY_L2);
		if (err != 0) {
			return err;
		}

		if (k_sem_take(&sem_sec, PROC_TIMEOUT) != 0) {
			return -ETIMEDOUT;
		}

		(void)bt_conn_get_info(uc.conn, &info);
		if (info.security.level < BT_SECURITY_L2) {
			return -EACCES;
		}
	}

	k_sem_reset(&sem_proc);
	err = bt_cap_initiator_unicast_discover(uc.conn);
	err = err ?: wait_proc(PROC_TIMEOUT);
	if (err != 0) {
		return err;
	}

	k_sem_reset(&sem_proc);
	err = bt_bap_unicast_client_discover(uc.conn, BT_AUDIO_DIR_SINK);
	err = err ?: wait_proc(PROC_TIMEOUT);
	if (err != 0) {
		return err;
	}

	k_sem_reset(&sem_proc);
	err = bt_bap_unicast_client_discover(uc.conn, BT_AUDIO_DIR_SOURCE);
	err = err ?: wait_proc(PROC_TIMEOUT);
	if (err != 0 && err != -ENODATA) {
		LOG_WRN("Source ASE discovery failed: %d", err);
	}

	uc.discovered = true;

	return 0;
}

/* --- Streams --- */

int unicast_close(void)
{
	struct bt_cap_stream *streams[2];
	size_t count = 0;
	int err;

	if (uc.group == NULL) {
		return 0;
	}

	if (uc.snk.bap_stream.ep != NULL) {
		streams[count++] = &uc.snk;
	}

	if (uc.src.bap_stream.ep != NULL) {
		streams[count++] = &uc.src;
	}

	if (count > 0) {
		const struct bt_cap_unicast_audio_stop_param param = {
			.type = BT_CAP_SET_TYPE_AD_HOC,
			.count = count,
			.streams = streams,
			.release = true,
		};

		k_sem_reset(&sem_proc);
		err = bt_cap_initiator_unicast_audio_stop(&param);
		err = err ?: wait_proc(PROC_TIMEOUT);
		if (err != 0) {
			LOG_WRN("Stop failed: %d", err);
		}
	}

	err = bt_cap_unicast_group_delete(uc.group);
	if (err != 0) {
		LOG_WRN("Group delete failed: %d", err);
	}

	uc.group = NULL;
	uc.streaming = false;
	cli_evt("source_stream_closed", "id=%u", uc.dev_id);

	return 0;
}

static int update_context(const struct unicast_cfg *cfg)
{
	struct bt_cap_unicast_audio_update_stream_param params[2];
	struct bt_cap_unicast_audio_update_param param = {
		.type = BT_CAP_SET_TYPE_AD_HOC,
		.stream_params = params,
	};
	int err;

	err = bt_audio_codec_cfg_meta_set_stream_context(&uc.preset.codec_cfg, cfg->ctx);
	if (err < 0) {
		return err;
	}

	params[param.count].stream = &uc.snk;
	params[param.count].meta = uc.preset.codec_cfg.meta;
	params[param.count].meta_len = uc.preset.codec_cfg.meta_len;
	param.count++;

	if (uc.cur.bidir) {
		params[param.count] = params[0];
		params[param.count].stream = &uc.src;
		param.count++;
	}

	k_sem_reset(&sem_proc);
	err = bt_cap_initiator_unicast_audio_update(&param);
	err = err ?: wait_proc(PROC_TIMEOUT);
	if (err != 0) {
		return err;
	}

	uc.cur.ctx = cfg->ctx;
	uc.cur.tone = cfg->tone;
	audio_tx_set_tone(&uc.snk, &cfg->tone);
	cli_evt("source_stream_updated", "id=%u ctx=%s tone=%u", uc.dev_id, audio_ctx_str(cfg->ctx),
		cfg->tone.freq_hz);

	return 0;
}

/* CIG with the sink CIS (to the hearing aid) and, bidirectional, the source CIS (microphone) */
static int create_group(bool bidir)
{
	struct bt_cap_unicast_group_stream_param tx_param = {
		.qos_cfg = &uc.preset.qos,
		.stream = &uc.snk,
	};
	struct bt_cap_unicast_group_stream_param rx_param = {
		.qos_cfg = &uc.preset.qos,
		.stream = &uc.src,
	};
	struct bt_cap_unicast_group_stream_pair_param pair = {
		.tx_param = &tx_param,
		.rx_param = bidir ? &rx_param : NULL,
	};
	struct bt_cap_unicast_group_param group_param = {
		.params_count = 1,
		.params = &pair,
		.packing = BT_ISO_PACKING_SEQUENTIAL,
	};

	return bt_cap_unicast_group_create(&group_param, &uc.group);
}

/* Configures, enables and starts the streams (CAP), waiting for the procedure to complete */
static int start_streams(bool bidir)
{
	struct bt_cap_unicast_audio_start_stream_param streams[2] = {
		{
			.member.member = uc.conn,
			.stream = &uc.snk,
			.ep = uc.snk_ep,
			.codec_cfg = &uc.preset.codec_cfg,
		},
		{
			.member.member = uc.conn,
			.stream = &uc.src,
			.ep = uc.src_ep,
			.codec_cfg = &uc.preset.codec_cfg,
		},
	};
	struct bt_cap_unicast_audio_start_param start = {
		.type = BT_CAP_SET_TYPE_AD_HOC,
		.count = bidir ? 2 : 1,
		.stream_params = streams,
	};
	int err;

	k_sem_reset(&sem_proc);
	err = bt_cap_initiator_unicast_audio_start(&start);

	return err != 0 ? err : wait_proc(START_TIMEOUT);
}

int unicast_open(const struct unicast_cfg *req)
{
	struct unicast_cfg cfg = *req;
	const struct bt_bap_lc3_preset *preset;
	int err;

	preset = preset_find(PRESET_UNICAST, cfg.preset, &cfg.preset);
	if (preset == NULL) {
		return -EINVAL;
	}

	err = unicast_ensure_profile();
	if (err != 0) {
		return err;
	}

	if (uc.snk_ep == NULL) {
		return -ENOTSUP;
	}

	cfg.bidir = cfg.bidir && uc.src_ep != NULL;

	/* Same codec configuration and directions: only switch the streaming context */
	if (uc.streaming && uc.cur.preset == cfg.preset && uc.cur.bidir == cfg.bidir) {
		return update_context(&cfg);
	}

	(void)unicast_close();

	uc.preset = *preset;
	err = bt_audio_codec_cfg_meta_set_stream_context(&uc.preset.codec_cfg, cfg.ctx);
	if (err < 0) {
		return err;
	}

	err = create_group(cfg.bidir);
	if (err != 0) {
		return err;
	}

	uc.cur = cfg; /* read by the stream started callback (tone) */
	atomic_set(&uc.rx_count, 0);
	atomic_set(&uc.rx_lost, 0);
	uc.rx_last_seq = -1;

	err = start_streams(cfg.bidir);
	if (err != 0) {
		(void)unicast_close();
		return err;
	}

	uc.streaming = true;
	cli_evt("source_stream_open", "id=%u ctx=%s preset=%s bidir=%d tone=%u", uc.dev_id,
		audio_ctx_str(cfg.ctx), cfg.preset, cfg.bidir, cfg.tone.freq_hz);

	return 0;
}

/* --- Bluetooth callbacks --- */

static void stream_started(struct bt_bap_stream *bap_stream)
{
	struct bt_cap_stream *stream = CONTAINER_OF(bap_stream, struct bt_cap_stream, bap_stream);

	if (stream == &uc.snk) {
		int err = audio_tx_start(stream, &uc.cur.tone);

		if (err != 0) {
			LOG_ERR("TX start failed: %d", err);
		}
	}

	cli_evt("source_stream_started", "id=%u dir=%s", uc.dev_id,
		stream == &uc.snk ? "sink" : "source");
}

static void stream_stopped(struct bt_bap_stream *bap_stream, uint8_t reason)
{
	struct bt_cap_stream *stream = CONTAINER_OF(bap_stream, struct bt_cap_stream, bap_stream);

	audio_tx_stop(stream);
	cli_evt("source_stream_stopped", "id=%u dir=%s reason=0x%02x", uc.dev_id,
		stream == &uc.snk ? "sink" : "source", reason);
}

static void stream_sent(struct bt_bap_stream *bap_stream)
{
	audio_tx_sent(CONTAINER_OF(bap_stream, struct bt_cap_stream, bap_stream));
}

static void stream_recv(struct bt_bap_stream *bap_stream, const struct bt_iso_recv_info *info,
			struct net_buf *buf)
{
	ARG_UNUSED(bap_stream);
	ARG_UNUSED(buf);

	/* SDUs the controller never delivered show up only as gaps in the sequence numbers;
	 * delivered but marked lost/invalid count as lost too.
	 */
	if (uc.rx_last_seq >= 0) {
		const uint16_t gap = (uint16_t)(info->seq_num - (uint16_t)uc.rx_last_seq - 1U);

		if (gap > 0 && gap < 1000) {
			atomic_add(&uc.rx_lost, gap);
		}
	}

	uc.rx_last_seq = info->seq_num;
	if ((info->flags & BT_ISO_FLAGS_VALID) != 0) {
		atomic_inc(&uc.rx_count);
	} else {
		atomic_inc(&uc.rx_lost);
	}
}

static struct bt_bap_stream_ops stream_ops = {
	.started = stream_started,
	.stopped = stream_stopped,
	.sent = stream_sent,
	.recv = stream_recv,
};

static void bap_discover_cb(struct bt_conn *conn, int err, enum bt_audio_dir dir)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(dir);

	proc_done(err == BT_ATT_ERR_ATTRIBUTE_NOT_FOUND ? -ENODATA : err);
}

static void bap_endpoint_cb(struct bt_conn *conn, enum bt_audio_dir dir, struct bt_bap_ep *ep)
{
	ARG_UNUSED(conn);

	if (dir == BT_AUDIO_DIR_SINK && uc.snk_ep == NULL) {
		uc.snk_ep = ep;
	} else if (dir == BT_AUDIO_DIR_SOURCE && uc.src_ep == NULL) {
		uc.src_ep = ep;
	}
}

static struct bt_bap_unicast_client_cb bap_cb = {
	.discover = bap_discover_cb,
	.endpoint = bap_endpoint_cb,
};

static void cap_discovery_complete(struct bt_conn *conn, int err,
				   const struct bt_csip_set_coordinator_set_member *member,
				   const struct bt_csip_set_coordinator_csis_inst *csis_inst)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(member);
	ARG_UNUSED(csis_inst);

	proc_done(err);
}

static void cap_proc_complete(int err, struct bt_conn *conn)
{
	ARG_UNUSED(conn);

	proc_done(err);
}

static struct bt_cap_initiator_cb cap_cb = {
	.unicast_discovery_complete = cap_discovery_complete,
	.unicast_start_complete = cap_proc_complete,
	.unicast_update_complete = cap_proc_complete,
	.unicast_stop_complete = cap_proc_complete,
};

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	ARG_UNUSED(level);
	ARG_UNUSED(err);

	if (conn == uc.conn) {
		k_sem_give(&sem_sec);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);

	if (conn == uc.conn) {
		/* Unblock a procedure waiting on this link; the worker cleans up */
		proc_done(-ENOTCONN);
		k_sem_give(&sem_sec);
		phone_post_event(PH_EV_LINK_LOST, 0);
	}
}

BT_CONN_CB_DEFINE(unicast_conn_cb) = {
	.disconnected = disconnected,
	.security_changed = security_changed,
};

int unicast_init(void)
{
	int err;

	err = bt_cap_initiator_register_cb(&cap_cb);
	err = err ?: bt_bap_unicast_client_register_cb(&bap_cb);
	if (err != 0) {
		return err;
	}

	bt_cap_stream_ops_register(&uc.snk, &stream_ops);
	bt_cap_stream_ops_register(&uc.src, &stream_ops);

	return 0;
}
