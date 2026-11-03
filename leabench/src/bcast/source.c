/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * The Zephyr CAP broadcast source of a draft: metadata, BASE / BIS data, extended and periodic
 * advertising, start / stop, stream callbacks feeding the tones (audio_tx), TX power.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/cap.h>
#include <zephyr/bluetooth/audio/pbp.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci_vs.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>

#include "audio_tx.h"
#include "bcast.h"
#include "cli.h"
#include "hci_util.h"
#include "bcast/internal.h"

LOG_MODULE_REGISTER(bcast, LOG_LEVEL_INF);

/* A BIG the controller cannot fit into the air is never created and reports nothing: give up
 * after BIG_START_TIMEOUT (outside struct big, which big_init_draft() clears).
 */
#define BIG_START_TIMEOUT K_SECONDS(5)

/* Waiting for the streams of a stopped BIG to terminate (polled) */
#define BIG_STOP_TIMEOUT_MS 1000
#define POLL_MS             10

static struct k_work_delayable start_timeout[MAX_BIGS];

/* Serialises big_start / big_stop between the shell and the start timeout (system workqueue) */
static K_MUTEX_DEFINE(big_lock);

/* What the controller actually laid out, against what the BIG asked for: repetitions it lowered
 * on its own or SDUs split into several PDUs change the test (and the air time)
 */
static void check_layout(const struct big *big, struct bt_bap_stream *bap_stream)
{
	struct bt_iso_info info;

	if (bap_stream->iso == NULL || bt_iso_chan_get_info(bap_stream->iso, &info) != 0 ||
	    info.type != BT_ISO_CHAN_TYPE_BROADCASTER) {
		return;
	}

	if (info.broadcaster.irc != big->qos.rtn + 1U || info.broadcaster.bn != big_bn(&big->qos)) {
		cli_evt("broadcast_warning", "big=%d reason=layout rtn=%u rtn_air=%u bn=%u",
			big_number(big), big->qos.rtn, info.broadcaster.irc - 1U,
			info.broadcaster.bn);
	}
}

static void stream_started(struct bt_bap_stream *bap_stream)
{
	int i;
	struct big *big = big_of_stream(bap_stream, &i);
	int err;

	if (big == NULL) {
		return;
	}

	err = audio_tx_start(&big->bis[i].stream, &big->bis[i].tone);
	if (err != 0) {
		LOG_ERR("TX start BIG %d BIS %d failed: %d", big_number(big), i + 1, err);
	} else if (big_bn(&big->qos) > 1U) {
		audio_tx_set_depth(&big->bis[i].stream, big_bn(&big->qos));
	}

	if (big->started == 0) {
		check_layout(big, bap_stream);
	}

	if (++big->started == big->bis_count) {
		cli_evt("broadcast_started",
			"big=%d broadcast_id=0x%06X name=%s bis=%u subgroups=%u encrypted=%d "
			"airtime_us=%u",
			big_number(big), big->broadcast_id, CLI_Q(big->name), big->bis_count,
			big->sg_count, big->encrypted, big_airtime_us(big, &big->qos));
		warn_interval_mix(big);
		(void)k_work_cancel_delayable(&start_timeout[big - bcast_bigs]);
	}
}

static void start_timeout_handler(struct k_work *work)
{
	const int b = k_work_delayable_from_work(work) - start_timeout;
	struct big *big = &bcast_bigs[b];

	k_mutex_lock(&big_lock, K_FOREVER);
	if (big->state == BIG_STREAMING && big->started < big->bis_count) {
		cli_evt("broadcast_error",
			"big=%d reason=start_timeout started=%u bis=%u airtime_us=%u: no room in "
			"the air next to the other broadcasts (see 'broadcast budget')",
			b + 1, big->started, big->bis_count, big_airtime_us(big, &big->qos));
		big_stop(big);
		big->last_err = -ETIMEDOUT;
	}
	k_mutex_unlock(&big_lock);
}

static void stream_stopped(struct bt_bap_stream *bap_stream, uint8_t reason)
{
	int i;
	struct big *big = big_of_stream(bap_stream, &i);

	if (big == NULL) {
		return;
	}

	audio_tx_stop(&big->bis[i].stream);
	if (big->started == 0) {
		return;
	}

	big->started--;
	if (big->started == 0) {
		cli_evt("broadcast_stopped", "big=%d broadcast_id=0x%06X reason=0x%02x",
			big_number(big), big->broadcast_id, reason);
	}
}

static void stream_sent(struct bt_bap_stream *bap_stream)
{
	audio_tx_sent(CONTAINER_OF(bap_stream, struct bt_cap_stream, bap_stream));
}

/* Registered on every BIS stream (bcast_init, big_init_draft) */
struct bt_bap_stream_ops bcast_stream_ops = {
	.started = stream_started,
	.stopped = stream_stopped,
	.sent = stream_sent,
};

int build_metadata(struct subgroup *sg)
{
	struct bt_audio_codec_cfg *cfg = &sg->preset.codec_cfg;
	int err;

	cfg->meta_len = 0;
	err = bt_audio_codec_cfg_meta_set_stream_context(cfg, sg->ctx);
	if (err >= 0 && sg->lang[0] != '\0') {
		err = bt_audio_codec_cfg_meta_set_lang(cfg, (const uint8_t *)sg->lang);
	}

	if (err >= 0 && sg->program[0] != '\0') {
		err = bt_audio_codec_cfg_meta_set_program_info(cfg, (const uint8_t *)sg->program,
							       strlen(sg->program));
	}

	return err < 0 ? err : 0;
}

/* BIS-specific Codec_Specific_Configuration LTVs (override the subgroup's values) */
static size_t build_bis_data(struct bis *bis)
{
	uint8_t *p = bis->data;

	if (bis->rate_hz != 0) {
		*p++ = 2;
		*p++ = BT_AUDIO_CODEC_CFG_FREQ;
		*p++ = (uint8_t)bt_audio_codec_cfg_freq_hz_to_freq(bis->rate_hz);
	}

	if (bis->octets != 0) {
		*p++ = 3;
		*p++ = BT_AUDIO_CODEC_CFG_FRAME_LEN;
		sys_put_le16(bis->octets, p);
		p += 2;
	}

	*p++ = 5;
	*p++ = BT_AUDIO_CODEC_CFG_CHAN_ALLOC;
	sys_put_le32(bis->loc, p);
	p += 4;

	return p - bis->data;
}

int update_adv_data(struct big *big)
{
	NET_BUF_SIMPLE_DEFINE(id_buf, BT_UUID_SIZE_16 + BT_AUDIO_BROADCAST_ID_SIZE);
	NET_BUF_SIMPLE_DEFINE(pba_buf, BT_UUID_SIZE_16 + 2 + PROG_MAX_LEN + 2);
	NET_BUF_SIMPLE_DEFINE(base_buf, 255);
	const char *program = big->sg[0].program;
	struct bt_data ad[3];
	struct bt_data per_ad;
	size_t ad_count = 0;
	int err;

	net_buf_simple_add_le16(&id_buf, BT_UUID_BROADCAST_AUDIO_VAL);
	net_buf_simple_add_le24(&id_buf, big->broadcast_id);
	ad[ad_count++] = (struct bt_data)BT_DATA(BT_DATA_SVC_DATA16, id_buf.data, id_buf.len);
	ad[ad_count++] =
		(struct bt_data)BT_DATA(BT_DATA_BROADCAST_NAME, big->name, strlen(big->name));

	if (big->pbp_enabled) {
		enum bt_pbp_announcement_feature features =
			big->pbp_hq ? BT_PBP_ANNOUNCEMENT_FEATURE_HIGH_QUALITY
				    : BT_PBP_ANNOUNCEMENT_FEATURE_STANDARD_QUALITY;
		uint8_t meta[2 + PROG_MAX_LEN];
		size_t meta_len = 0;

		if (big->encrypted) {
			features |= BT_PBP_ANNOUNCEMENT_FEATURE_ENCRYPTION;
		}

		if (program[0] != '\0') {
			meta[0] = strlen(program) + 1;
			meta[1] = BT_AUDIO_METADATA_TYPE_PROGRAM_INFO;
			/* LTV value: length-prefixed, no terminator */
			/* NOLINTNEXTLINE(bugprone-not-null-terminated-result) */
			memcpy(&meta[2], program, strlen(program));
			meta_len = meta[0] + 1;
		}

		err = bt_pbp_get_announcement(meta_len > 0 ? meta : NULL, meta_len, features,
					      &pba_buf);
		if (err != 0) {
			return err;
		}

		ad[ad_count++] =
			(struct bt_data)BT_DATA(BT_DATA_SVC_DATA16, pba_buf.data, pba_buf.len);
	}

	err = bt_le_ext_adv_set_data(big->adv, ad, ad_count, NULL, 0);
	if (err != 0) {
		return err;
	}

	err = bt_cap_initiator_broadcast_get_base(big->source, &base_buf);
	if (err != 0) {
		return err;
	}

	per_ad = (struct bt_data)BT_DATA(BT_DATA_SVC_DATA16, base_buf.data, base_buf.len);

	return bt_le_per_adv_set_data(big->adv, &per_ad, 1);
}

/* -EBADMSG if the source is still stopping: it is kept and deleted by the next release */
static int big_release(struct big *big)
{
	if (big->source != NULL) {
		const int err = bt_cap_initiator_broadcast_audio_delete(big->source);

		if (err != 0) {
			LOG_WRN("BIG %d: source delete failed: %d", big_number(big), err);
			return err;
		}
		big->source = NULL;
	}

	if (big->adv != NULL) {
		(void)bt_le_per_adv_stop(big->adv);
		(void)bt_le_ext_adv_stop(big->adv);
		(void)bt_le_ext_adv_delete(big->adv);
		big->adv = NULL;
	}

	return 0;
}

/* The BIG transmits with the power of its advertising set (Zephyr controller) */
int apply_txpower(struct big *big, int8_t *selected)
{
	int8_t sel = 0;
	int err;

	if (big->txpower == TXPOWER_DEFAULT || big->adv == NULL) {
		return 0;
	}

	err = hci_set_tx_power(BT_HCI_VS_LL_HANDLE_TYPE_ADV, bt_le_ext_adv_get_index(big->adv),
			       big->txpower, &sel);
	if (selected != NULL) {
		*selected = sel;
	}

	return err;
}

static int big_start_locked(struct big *big)
{
	struct bt_cap_initiator_broadcast_stream_param stream_params[MAX_BIS] = {0};
	struct bt_cap_initiator_broadcast_subgroup_param sg_params[MAX_SG] = {0};
	struct bt_cap_initiator_broadcast_stream_param *ordered = stream_params;
	struct bt_cap_initiator_broadcast_create_param create = {0};
	int err;

	const int sg_count = big->sg_count;
	const int bis_count = big->bis_count;

	/* Model invariant (args.c); the arrays above are sized by it */
	if (sg_count > MAX_SG || bis_count > MAX_BIS) {
		return -EINVAL;
	}

	/* A source of an earlier run that was still stopping */
	err = big_release(big);
	if (err != 0) {
		return -EBUSY;
	}

	big_effective_qos(big, &big->qos);

	/* Streams are passed grouped by subgroup; BIS indexes follow that order */
	for (int s = 0; s < sg_count; s++) {
		err = build_metadata(&big->sg[s]);
		if (err != 0) {
			return err;
		}

		sg_params[s].codec_cfg = &big->sg[s].preset.codec_cfg;
		sg_params[s].stream_params = ordered;
		for (int i = 0; i < bis_count; i++) {
			struct bis *bis = &big->bis[i];

			if (bis->sg != s) {
				continue;
			}

			ordered->stream = &bis->stream;
			ordered->data = bis->data;
			ordered->data_len = build_bis_data(bis);
			ordered++;
			sg_params[s].stream_count++;
		}
	}

	create.qos = &big->qos;
	create.subgroup_params = sg_params;
	create.subgroup_count = big->sg_count;
	create.packing = big->packing;
	create.encryption = big->encrypted;
	memcpy(create.broadcast_code, big->code, sizeof(create.broadcast_code));

	err = bt_le_ext_adv_create(BT_BAP_ADV_PARAM_BROADCAST_FAST, NULL, &big->adv);
	err = err
		      ?: bt_le_per_adv_set_param(
				 big->adv,
				 BT_LE_PER_ADV_PARAM(BT_GAP_MS_TO_PER_ADV_INTERVAL(big->pa_ms),
						     BT_GAP_MS_TO_PER_ADV_INTERVAL(big->pa_ms),
						     BT_LE_PER_ADV_OPT_NONE));
	err = err ?: bt_cap_initiator_broadcast_audio_create(&create, &big->source);
	err = err ?: apply_txpower(big, NULL);
	if (err != 0) {
		(void)big_release(big);
		return err;
	}

	/* Advertising and periodic advertising first, the BIG last: the controller places a new
	 * BIG after the periodic advertising events already scheduled. Started the other way
	 * round, the periodic advertising of one broadcast landed on the events of another BIG
	 * and took one of its events every PA interval (60 ms: ~17 % loss).
	 */
	big->started = 0;
	err = update_adv_data(big);
	err = err ?: bt_le_ext_adv_start(big->adv, BT_LE_EXT_ADV_START_DEFAULT);
	err = err ?: bt_le_per_adv_start(big->adv);
	err = err ?: bt_cap_initiator_broadcast_audio_start(big->source, big->adv);
	if (err != 0) {
		(void)bt_cap_initiator_broadcast_audio_stop(big->source);
		(void)big_release(big);
		return err;
	}

	big->state = BIG_STREAMING;
	k_work_reschedule(&start_timeout[big - bcast_bigs], BIG_START_TIMEOUT);

	return 0;
}

int big_start(struct big *big)
{
	int err;

	k_mutex_lock(&big_lock, K_FOREVER);
	err = big_start_locked(big);
	k_mutex_unlock(&big_lock);

	return err;
}

void big_stop(struct big *big)
{
	/* Recursive: the start timeout calls this with the lock held */
	k_mutex_lock(&big_lock, K_FOREVER);
	if (big->state != BIG_STREAMING) {
		k_mutex_unlock(&big_lock);
		return;
	}

	(void)k_work_cancel_delayable(&start_timeout[big - bcast_bigs]);
	(void)bt_cap_initiator_broadcast_audio_stop(big->source);
	/* Streams stop asynchronously; let the BIG terminate before deleting the source */
	for (int t = 0; t < BIG_STOP_TIMEOUT_MS && big->started > 0; t += POLL_MS) {
		k_sleep(K_MSEC(POLL_MS));
	}

	(void)big_release(big);
	big->state = BIG_DRAFT;
	big->was_started = true;
	k_mutex_unlock(&big_lock);
}

void bcast_reset(void)
{
	for (int b = 0; b < MAX_BIGS; b++) {
		big_stop(&bcast_bigs[b]);
		bcast_bigs[b].state = BIG_NONE;
	}
}

void bcast_stop_all(void)
{
	for (int b = 0; b < MAX_BIGS; b++) {
		big_stop(&bcast_bigs[b]);
	}
}

void bcast_counts(int *active, int *configured)
{
	*active = 0;
	*configured = 0;
	for (int b = 0; b < MAX_BIGS; b++) {
		*active += bcast_bigs[b].state == BIG_STREAMING;
		*configured += bcast_bigs[b].state != BIG_NONE;
	}
}

int bcast_init(void)
{
	for (int b = 0; b < MAX_BIGS; b++) {
		k_work_init_delayable(&start_timeout[b], start_timeout_handler);
		for (int i = 0; i < MAX_BIS; i++) {
			bt_cap_stream_ops_register(&bcast_bigs[b].bis[i].stream, &bcast_stream_ops);
		}
	}

	return 0;
}
