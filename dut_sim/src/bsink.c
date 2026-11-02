/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Broadcast (Auracast) sink: scans for a broadcast source (optionally by Broadcast Name), syncs
 * to its periodic advertising and BIG (all BISes, up to 2) and checks the audio on each BIS.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/pbp.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>

#include "cli.h"
#include "dut.h"

LOG_MODULE_REGISTER(bsink, LOG_LEVEL_INF);

#define MAX_BIS       CONFIG_BT_BAP_BROADCAST_SNK_STREAM_COUNT
#define NAME_LEN      33
#define PA_SYNC_TRIES 3

enum state {
	ST_IDLE,
	ST_SCANNING,
	ST_PA_SYNCING,
	ST_PA_SYNCED,
	ST_BIG_SYNCING,
	ST_STREAMING,
};

static struct {
	enum state state;
	char target[NAME_LEN];
	uint8_t code[BT_ISO_BROADCAST_CODE_SIZE];
	bool have_code;
	uint32_t broadcast_id;
	char name[NAME_LEN];
	struct bt_le_per_adv_sync *pa_sync;
	struct bt_bap_broadcast_sink *sink;
	/* Stopped but still streaming: deleted once its BIG has stopped (stale_work) */
	struct bt_bap_broadcast_sink *stale;
	uint32_t bis_indexes;
	bool base_received;
	bool biginfo_received;
	bool encrypted;
	uint32_t want_bis; /* BIS indexes requested with 'bcast scan bis=', 0: all */
	struct bt_bap_stream streams[MAX_BIS];
	uint8_t stream_bis[MAX_BIS]; /* BIS index carried by each stream */
	struct audio_rx rx[MAX_BIS];
	uint8_t started;
	uint8_t pa_tries;
	/* From the BASE, indexed by BIS index */
	uint8_t bis_sg[BT_ISO_BIS_INDEX_MAX + 1];
	char bis_lang[BT_ISO_BIS_INDEX_MAX + 1][BT_AUDIO_LANG_SIZE + 1];
} bs;

const char *bsink_state_str(void)
{
	static const char *const names[] = {"idle",      "scanning",    "pa_syncing",
					    "pa_synced", "big_syncing", "streaming"};

	return names[bs.state];
}

static void try_big_sync(void)
{
	struct bt_bap_stream *streams[MAX_BIS];
	uint32_t bitfield = 0;
	uint8_t n = 0;
	int err;

	if (bs.state != ST_PA_SYNCED || !bs.base_received || !bs.biginfo_received ||
	    bs.sink == NULL) {
		return;
	}

	if (bs.encrypted && !bs.have_code) {
		cli_evt("dut_bcast_error", "reason=code_required");
		return;
	}

	for (uint8_t i = BT_ISO_BIS_INDEX_MIN; i <= BT_ISO_BIS_INDEX_MAX && n < MAX_BIS; i++) {
		const uint32_t bit = BT_ISO_BIS_INDEX_BIT(i);

		if ((bs.bis_indexes & bit) && (bs.want_bis == 0 || (bs.want_bis & bit))) {
			bitfield |= bit;
			bs.stream_bis[n] = i;
			streams[n] = &bs.streams[n];
			n++;
		}
	}

	if (n == 0) {
		cli_evt("dut_bcast_error", "reason=no_such_bis");
		return;
	}

	err = bt_bap_broadcast_sink_sync(bs.sink, bitfield, streams, bs.code);
	if (err != 0) {
		cli_evt("dut_bcast_error", "reason=sync_failed err=%d", err);
		return;
	}

	bs.state = ST_BIG_SYNCING;
}

/* --- Scanning --- */

struct adv_info {
	bool is_broadcast;
	uint32_t broadcast_id;
	char name[NAME_LEN];
	bool pbp;
	uint8_t pbp_features;
};

static bool parse_adv(struct bt_data *data, void *user_data)
{
	struct adv_info *info = user_data;
	struct bt_uuid_16 uuid;

	switch (data->type) {
	case BT_DATA_BROADCAST_NAME:
		memcpy(info->name, data->data, MIN(data->data_len, NAME_LEN - 1));
		break;
	case BT_DATA_SVC_DATA16:
		if (data->data_len < BT_UUID_SIZE_16 ||
		    !bt_uuid_create(&uuid.uuid, data->data, BT_UUID_SIZE_16)) {
			break;
		}

		if (bt_uuid_cmp(&uuid.uuid, BT_UUID_BROADCAST_AUDIO) == 0 &&
		    data->data_len >= BT_UUID_SIZE_16 + BT_AUDIO_BROADCAST_ID_SIZE) {
			info->is_broadcast = true;
			info->broadcast_id = sys_get_le24(data->data + BT_UUID_SIZE_16);
		} else if (bt_uuid_cmp(&uuid.uuid, BT_UUID_PBA) == 0 &&
			   data->data_len > BT_UUID_SIZE_16) {
			info->pbp = true;
			info->pbp_features = data->data[BT_UUID_SIZE_16];
		}
		break;
	default:
		break;
	}

	return true;
}

static void scan_recv(const struct bt_le_scan_recv_info *info, struct net_buf_simple *ad)
{
	struct bt_le_per_adv_sync_param param = {0};
	struct adv_info adv = {0};
	char addr[CLI_ADDR_STR_LEN];
	int err;

	if (bs.state != ST_SCANNING || info->interval == 0U) {
		return;
	}

	bt_data_parse(ad, parse_adv, &adv);
	if (!adv.is_broadcast) {
		return;
	}

	if (bs.target[0] != '\0' && strcasecmp(bs.target, adv.name) != 0) {
		return;
	}

	cli_addr_str(info->addr, addr, sizeof(addr));
	cli_evt("dut_bcast_found",
		"broadcast_id=0x%06X name=%s addr=%s sid=%u pbp=%d pbp_encrypted=%d pbp_sq=%d "
		"pbp_hq=%d rssi=%d",
		adv.broadcast_id, CLI_Q(adv.name[0] != '\0' ? adv.name : "none"), addr, info->sid,
		adv.pbp, !!(adv.pbp_features & BT_PBP_ANNOUNCEMENT_FEATURE_ENCRYPTION),
		!!(adv.pbp_features & BT_PBP_ANNOUNCEMENT_FEATURE_STANDARD_QUALITY),
		!!(adv.pbp_features & BT_PBP_ANNOUNCEMENT_FEATURE_HIGH_QUALITY), info->rssi);

	/* Scanning goes on until the sync is established: the controller needs it to receive the
	 * AUX_ADV_IND with the SyncInfo (stopped here, the sync sometimes waited forever)
	 */
	bt_addr_le_copy(&param.addr, info->addr);
	param.sid = info->sid;
	param.skip = 0;
	param.timeout = 1000; /* 10 s */
	dut_adv_hold(true);
	err = bt_le_per_adv_sync_create(&param, &bs.pa_sync);
	if (err != 0) {
		(void)bt_le_scan_stop();
		dut_adv_hold(false);
		cli_evt("dut_bcast_error", "reason=pa_sync_create err=%d", err);
		bs.state = ST_IDLE;
		return;
	}

	bs.broadcast_id = adv.broadcast_id;
	memcpy(bs.name, adv.name, sizeof(bs.name));
	bs.state = ST_PA_SYNCING;
}

static struct bt_le_scan_cb scan_cb = {
	.recv = scan_recv,
};

/* --- PA sync --- */

static void pa_synced(struct bt_le_per_adv_sync *sync, struct bt_le_per_adv_sync_synced_info *info)
{
	int err;

	ARG_UNUSED(info);

	if (sync != bs.pa_sync) {
		return;
	}

	bs.state = ST_PA_SYNCED;
	(void)bt_le_scan_stop();
	dut_adv_hold(false);
	cli_evt("dut_bcast_pa_synced", "broadcast_id=0x%06X", bs.broadcast_id);

	err = bt_bap_broadcast_sink_create(sync, bs.broadcast_id, &bs.sink);
	if (err != 0) {
		cli_evt("dut_bcast_error", "reason=sink_create err=%d", err);
	}
}

static void pa_term(struct bt_le_per_adv_sync *sync,
		    const struct bt_le_per_adv_sync_term_info *info)
{
	if (sync != bs.pa_sync) {
		return;
	}

	bs.pa_sync = NULL;
	if (bs.state == ST_PA_SYNCING) {
		/* Establishing the periodic sync can fail on a busy channel: look again (the scan
		 * is still running)
		 */
		const int scan_err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, NULL);

		if (++bs.pa_tries < PA_SYNC_TRIES && (scan_err == 0 || scan_err == -EALREADY)) {
			LOG_WRN("PA sync failed (0x%02x), retrying", info->reason);
			bs.state = ST_SCANNING;
			return;
		}

		(void)bt_le_scan_stop();
		dut_adv_hold(false);
		cli_evt("dut_bcast_error", "reason=pa_sync_failed hci=0x%02x tries=%u",
			info->reason, bs.pa_tries);
		bs.state = ST_IDLE;
	} else if (bs.state != ST_STREAMING) {
		cli_evt("dut_bcast_pa_lost", "reason=0x%02x", info->reason);
	}
}

static struct bt_le_per_adv_sync_cb pa_cb = {
	.synced = pa_synced,
	.term = pa_term,
};

/* --- Broadcast sink --- */

static bool find_lang(struct bt_data *data, void *user_data)
{
	if (data->type == BT_AUDIO_METADATA_TYPE_LANG && data->data_len == BT_AUDIO_LANG_SIZE) {
		memcpy(user_data, data->data, BT_AUDIO_LANG_SIZE);
		return false;
	}

	return true;
}

/* Remembers subgroup number and language of every BIS index */
static bool base_subgroup_cb(const struct bt_bap_base_subgroup *subgroup, void *user_data)
{
	uint8_t *sg_number = user_data;
	char lang[BT_AUDIO_LANG_SIZE + 1] = "";
	uint32_t indexes = 0;
	uint8_t *meta;
	int meta_len;

	(*sg_number)++;
	meta_len = bt_bap_base_get_subgroup_codec_meta(subgroup, &meta);
	if (meta_len > 0) {
		(void)bt_audio_data_parse(meta, meta_len, find_lang, lang);
	}

	(void)bt_bap_base_subgroup_get_bis_indexes(subgroup, &indexes);
	for (uint8_t i = BT_ISO_BIS_INDEX_MIN; i <= BT_ISO_BIS_INDEX_MAX; i++) {
		if (indexes & BT_ISO_BIS_INDEX_BIT(i)) {
			bs.bis_sg[i] = *sg_number;
			memcpy(bs.bis_lang[i], lang, sizeof(lang));
		}
	}

	return true;
}

static void base_recv(struct bt_bap_broadcast_sink *sink, const struct bt_bap_base *base,
		      size_t base_size)
{
	ARG_UNUSED(sink);
	ARG_UNUSED(base_size);

	if (bs.base_received) {
		return;
	}

	if (bt_bap_base_get_bis_indexes(base, &bs.bis_indexes) != 0) {
		return;
	}

	uint8_t sg_number = 0;

	memset(bs.bis_lang, 0, sizeof(bs.bis_lang));
	(void)bt_bap_base_foreach_subgroup(base, base_subgroup_cb, &sg_number);
	bs.base_received = true;
	cli_evt("dut_bcast_base", "bis_count=%u subgroups=%d",
		sys_count_bits(&bs.bis_indexes, sizeof(bs.bis_indexes)),
		bt_bap_base_get_subgroup_count(base));
	try_big_sync();
}

static void syncable(struct bt_bap_broadcast_sink *sink, const struct bt_iso_biginfo *biginfo)
{
	ARG_UNUSED(sink);

	if (bs.biginfo_received) {
		return;
	}

	bs.biginfo_received = true;
	bs.encrypted = biginfo->encryption;
	cli_evt("dut_bcast_biginfo", "encrypted=%d bis=%u", biginfo->encryption, biginfo->num_bis);
	try_big_sync();
}

static void sink_started(struct bt_bap_broadcast_sink *sink)
{
	ARG_UNUSED(sink);
}

static bool collect_src_id(const struct bt_bap_scan_delegator_recv_state *state, void *user_data)
{
	struct {
		uint8_t ids[CONFIG_BT_BAP_SCAN_DELEGATOR_RECV_STATE_COUNT];
		size_t n;
	} *c = user_data;

	if (c->n < ARRAY_SIZE(c->ids)) {
		c->ids[c->n++] = state->src_id;
	}

	return false;
}

/* Deleting a sink keeps the BASS receive state it added; without this the next sink finds no
 * free one ("Failed to add sync as Receive State: -12")
 */
static void release_recv_states(void)
{
	struct {
		uint8_t ids[CONFIG_BT_BAP_SCAN_DELEGATOR_RECV_STATE_COUNT];
		size_t n;
	} c = {.n = 0};

	bt_bap_scan_delegator_foreach_state(collect_src_id, &c);
	for (size_t i = 0; i < c.n; i++) {
		uint32_t no_bis[BT_BAP_BASS_MAX_SUBGROUPS] = {0};

		/* A state still marked synced cannot be removed */
		(void)bt_bap_scan_delegator_set_bis_sync_state(c.ids[i], no_bis);
		(void)bt_bap_scan_delegator_set_pa_state(c.ids[i], BT_BAP_PA_STATE_NOT_SYNCED);
		(void)bt_bap_scan_delegator_rem_src(c.ids[i]);
	}
}

/* Outside the sink's callback, which is still iterating its listeners */
static void stale_delete(struct k_work *work)
{
	ARG_UNUSED(work);

	if (bs.stale != NULL && bt_bap_broadcast_sink_delete(bs.stale) == 0) {
		bs.stale = NULL;
		release_recv_states();
	}
}

static K_WORK_DEFINE(stale_work, stale_delete);

static void sink_stopped(struct bt_bap_broadcast_sink *sink, uint8_t reason)
{
	if (sink == bs.stale) {
		(void)k_work_submit(&stale_work);
		return;
	}

	cli_evt("dut_bcast_stopped", "reason=0x%02x%s", reason,
		reason == BT_HCI_ERR_TERM_DUE_TO_MIC_FAIL ? " cause=wrong_code" : "");
	if (bs.state == ST_BIG_SYNCING || bs.state == ST_STREAMING) {
		bs.state = ST_PA_SYNCED;
	}
}

static struct bt_bap_broadcast_sink_cb sink_cb = {
	.base_recv = base_recv,
	.syncable = syncable,
	.started = sink_started,
	.stopped = sink_stopped,
};

static int bis_index(const struct bt_bap_stream *stream)
{
	return stream - bs.streams;
}

static void stream_started(struct bt_bap_stream *stream)
{
	const int i = bis_index(stream);
	const uint8_t bis = bs.stream_bis[i];
	const int freq = bt_audio_codec_cfg_get_freq(stream->codec_cfg);
	char label[24];

	snprintf(label, sizeof(label), "bcast_bis%u", bis);
	(void)audio_rx_start(&bs.rx[i], stream->codec_cfg, label);

	bs.state = ST_STREAMING;
	bs.started++;
	cli_evt("dut_bcast_stream", "state=started bis=%u sg=%u sample_rate=%u octets=%d lang=%s",
		bis, bs.bis_sg[bis], freq >= 0 ? bt_audio_codec_cfg_freq_to_freq_hz(freq) : 0,
		bt_audio_codec_cfg_get_octets_per_frame(stream->codec_cfg),
		bs.bis_lang[bis][0] != '\0' ? bs.bis_lang[bis] : "none");
}

static void stream_stopped(struct bt_bap_stream *stream, uint8_t reason)
{
	const int i = bis_index(stream);

	audio_rx_stop(&bs.rx[i]);
	if (bs.started > 0) {
		bs.started--;
	}

	cli_evt("dut_bcast_stream", "state=stopped bis=%u reason=0x%02x", bs.stream_bis[i], reason);
}

static void stream_recv(struct bt_bap_stream *stream, const struct bt_iso_recv_info *info,
			struct net_buf *buf)
{
	audio_rx_frame(&bs.rx[bis_index(stream)], info, buf);
}

static struct bt_bap_stream_ops stream_ops = {
	.started = stream_started,
	.stopped = stream_stopped,
	.recv = stream_recv,
};

/* The scan delegator is required by the broadcast sink; remote requests are not supported */
static int pa_sync_req(struct bt_conn *conn,
		       const struct bt_bap_scan_delegator_recv_state *recv_state, bool past_avail,
		       uint16_t pa_interval)
{
	return -ENOTSUP;
}

static struct bt_bap_scan_delegator_cb delegator_cb = {
	.pa_sync_req = pa_sync_req,
};

int bsink_init(void)
{
	int err;

	err = bt_bap_scan_delegator_register(&delegator_cb);
	err = err ?: bt_bap_broadcast_sink_register_cb(&sink_cb);
	if (err != 0) {
		return err;
	}

	bt_le_per_adv_sync_cb_register(&pa_cb);
	bt_le_scan_cb_register(&scan_cb);
	for (size_t i = 0; i < ARRAY_SIZE(bs.streams); i++) {
		bt_bap_stream_cb_register(&bs.streams[i], &stream_ops);
	}

	return 0;
}

void bsink_reset(void)
{
	if (bs.state == ST_SCANNING || bs.state == ST_PA_SYNCING) {
		(void)bt_le_scan_stop();
	}

	if (bs.sink != NULL) {
		/* Stopping a synced BIG is asynchronous: the sink can only be deleted (which also
		 * frees its BASS receive state) after its `stopped` callback.
		 */
		(void)bt_bap_broadcast_sink_stop(bs.sink);
		if (bt_bap_broadcast_sink_delete(bs.sink) != 0) {
			/* One still stopping from before: try it once more, never lose it silently
			 */
			if (bs.stale != NULL && bt_bap_broadcast_sink_delete(bs.stale) != 0) {
				LOG_WRN("Broadcast sink %p still stopping, dropped",
					(void *)bs.stale);
			}
			bs.stale = bs.sink;
		} else {
			release_recv_states();
		}
		bs.sink = NULL;
	}

	if (bs.pa_sync != NULL) {
		(void)bt_le_per_adv_sync_delete(bs.pa_sync);
		bs.pa_sync = NULL;
	}

	dut_adv_hold(false);
	bs.state = ST_IDLE;
	bs.base_received = false;
	bs.biginfo_received = false;
	bs.started = 0;
	bs.pa_tries = 0;
}

/* --- Shell commands --- */

static int cmd_scan(const struct shell *sh, size_t argc, char **argv)
{
	const char *name = cli_kv(argc, argv, "name");
	const char *code = cli_kv(argc, argv, "code");
	const char *bis = cli_kv(argc, argv, "bis");
	uint32_t want = 0;
	int err;

	if (code != NULL && strlen(code) > BT_ISO_BROADCAST_CODE_SIZE) {
		return cli_err(sh, -EINVAL, "code max 16 characters");
	}

	/* bis=1,3: comma separated BIS indexes */
	for (const char *p = bis; p != NULL && *p != '\0';) {
		char *end;
		unsigned long idx = strtoul(p, &end, 10);

		if (end == p || idx < BT_ISO_BIS_INDEX_MIN || idx > BT_ISO_BIS_INDEX_MAX ||
		    (*end != ',' && *end != '\0')) {
			return cli_err(sh, -EINVAL, "bis=<index>[,<index>...]");
		}

		want |= BT_ISO_BIS_INDEX_BIT(idx);
		p = *end == ',' ? end + 1 : end;
	}

	bsink_reset();
	bs.want_bis = want;
	snprintf(bs.target, sizeof(bs.target), "%s", name != NULL ? name : "");
	memset(bs.code, 0, sizeof(bs.code));
	bs.have_code = code != NULL;
	if (code != NULL) {
		memcpy(bs.code, code, strlen(code));
	}

	err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, NULL);
	if (err != 0) {
		return cli_err(sh, err, "scan start failed");
	}

	bs.state = ST_SCANNING;
	cli_ok(sh, "target=%s", CLI_Q(bs.target[0] != '\0' ? bs.target : "any"));

	return 0;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	bsink_reset();
	cli_ok(sh, NULL);

	return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	cli_ok(sh, "state=%s broadcast_id=0x%06X name=%s encrypted=%d streams=%u",
	       bsink_state_str(), bs.broadcast_id, CLI_Q(bs.name[0] != '\0' ? bs.name : "none"),
	       bs.encrypted, bs.started);

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	bcast_cmds,
	SHELL_CMD_ARG(scan, NULL, "[name=<broadcast name>] [code=<broadcast code>] [bis=1,3]",
		      cmd_scan, 1, 3),
	SHELL_CMD_ARG(stop, NULL, "Leave the broadcast", cmd_stop, 1, 0),
	SHELL_CMD_ARG(status, NULL, "Broadcast sink state", cmd_status, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(bcast, &bcast_cmds, "Broadcast sink", NULL);
