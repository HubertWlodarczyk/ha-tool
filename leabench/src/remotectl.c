/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Device controls a companion app uses: presets (Hearing Access Service client) and the
 * microphone (Microphone Control, with its Audio Input Control for gain). Both are optional on
 * the device; discovery runs once per link from the phone worker thread.
 */

#include <errno.h>

#include <zephyr/bluetooth/audio/aics.h>
#include <zephyr/bluetooth/audio/has.h>
#include <zephyr/bluetooth/audio/micp.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "cli.h"
#include "phone.h"

LOG_MODULE_REGISTER(remotectl, LOG_LEVEL_INF);

/* GATT discovery of HAS / MICP on the device */
#define DISCOVERY_TIMEOUT K_SECONDS(10)

static struct bt_has *has;
static struct bt_micp_mic_ctlr *mic;
static struct bt_aics *mic_aics;
static K_SEM_DEFINE(sem_disc, 0, 1);
static int disc_err;
static uint8_t preset_count;

static int wait_disc(void)
{
	return k_sem_take(&sem_disc, DISCOVERY_TIMEOUT) != 0 ? -ETIMEDOUT : disc_err;
}

/* --- Hearing Access Service (presets) --- */

static void has_discover(struct bt_conn *conn, int err, struct bt_has *inst,
			 enum bt_has_hearing_aid_type type, enum bt_has_capabilities caps)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(caps);

	has = err == 0 ? inst : NULL;
	if (err == 0) {
		cli_evt("source_has", "type=%s",
			type == BT_HAS_HEARING_AID_TYPE_BINAURAL   ? "binaural"
			: type == BT_HAS_HEARING_AID_TYPE_MONAURAL ? "monaural"
								   : "banded");
	}

	disc_err = err;
	k_sem_give(&sem_disc);
}

static void has_preset_switch(struct bt_has *inst, int err, uint8_t index)
{
	ARG_UNUSED(inst);

	if (err == 0) {
		cli_evt("source_preset", "active=%u", index);
	} else {
		cli_evt("source_error", "op=preset err=%d", err);
	}
}

static void has_preset_read_rsp(struct bt_has *inst, int err,
				const struct bt_has_preset_record *record, bool is_last)
{
	ARG_UNUSED(inst);

	if (err != 0) {
		cli_evt("source_error", "op=preset_list err=%d", err);
		return;
	}

	preset_count++;
	cli_evt("source_preset_record", "idx=%u name=%s available=%d writable=%d", record->index,
		CLI_Q(record->name), !!(record->properties & BT_HAS_PROP_AVAILABLE),
		!!(record->properties & BT_HAS_PROP_WRITABLE));
	if (is_last) {
		cli_evt("source_preset_list", "count=%u", preset_count);
	}
}

static void has_preset_update(struct bt_has *inst, uint8_t index_prev,
			      const struct bt_has_preset_record *record, bool is_last)
{
	ARG_UNUSED(inst);
	ARG_UNUSED(index_prev);
	ARG_UNUSED(is_last);

	cli_evt("source_preset_record", "idx=%u name=%s available=%d updated=1", record->index,
		CLI_Q(record->name), !!(record->properties & BT_HAS_PROP_AVAILABLE));
}

static const struct bt_has_client_cb has_cb = {
	.discover = has_discover,
	.preset_switch = has_preset_switch,
	.preset_read_rsp = has_preset_read_rsp,
	.preset_update = has_preset_update,
};

/* --- Microphone Control --- */

static void mic_discover(struct bt_micp_mic_ctlr *ctlr, int err, uint8_t aics_count)
{
	struct bt_micp_included included;

	mic = err == 0 ? ctlr : NULL;
	mic_aics = NULL;
	if (err == 0 && aics_count > 0 && bt_micp_mic_ctlr_included_get(ctlr, &included) == 0 &&
	    included.aics_cnt > 0) {
		mic_aics = included.aics[0];
	}

	disc_err = err;
	k_sem_give(&sem_disc);
}

static void mic_mute(struct bt_micp_mic_ctlr *ctlr, int err, uint8_t mute)
{
	ARG_UNUSED(ctlr);

	if (err == 0) {
		cli_evt("source_mic", "mute=%u", mute);
	}
}

static void aics_state(struct bt_aics *inst, int err, int8_t gain, uint8_t mute, uint8_t mode)
{
	ARG_UNUSED(inst);

	if (err == 0) {
		cli_evt("source_mic_input", "gain=%d mute=%u mode=%u", gain, mute, mode);
	}
}

static struct bt_micp_mic_ctlr_cb mic_cb = {
	.discover = mic_discover,
	.mute = mic_mute,
	.aics_cb = {.state = aics_state},
};

/* --- API for the phone worker --- */

void remotectl_discover(struct bt_conn *conn)
{
	k_sem_reset(&sem_disc);
	if (bt_has_client_discover(conn) != 0 || wait_disc() != 0) {
		has = NULL;
	}

	k_sem_reset(&sem_disc);
	if (bt_micp_mic_ctlr_discover(conn, &mic) != 0 || wait_disc() != 0) {
		mic = NULL;
		mic_aics = NULL;
	}
}

void remotectl_forget(void)
{
	has = NULL;
	mic = NULL;
	mic_aics = NULL;
}

bool remotectl_has_presets(void)
{
	return has != NULL;
}

bool remotectl_has_mic(void)
{
	return mic != NULL;
}

int remotectl_preset(enum phone_preset_op op, uint8_t index)
{
	if (has == NULL) {
		return -ENOTSUP;
	}

	switch (op) {
	case PRESET_OP_LIST:
		preset_count = 0;
		return bt_has_client_presets_read(has, BT_HAS_PRESET_INDEX_FIRST, 255);
	case PRESET_OP_SET:
		return bt_has_client_preset_set(has, index, false);
	case PRESET_OP_NEXT:
		return bt_has_client_preset_next(has, false);
	case PRESET_OP_PREV:
		return bt_has_client_preset_prev(has, false);
	default:
		return -EINVAL;
	}
}

int remotectl_mic_mute(bool mute)
{
	if (mic == NULL) {
		return -ENOTSUP;
	}

	return mute ? bt_micp_mic_ctlr_mute(mic) : bt_micp_mic_ctlr_unmute(mic);
}

int remotectl_mic_gain(int8_t gain)
{
	return mic_aics != NULL ? bt_aics_gain_set(mic_aics, gain) : -ENOTSUP;
}

int remotectl_init(void)
{
	int err = bt_has_client_cb_register(&has_cb);

	return err ?: bt_micp_mic_ctlr_cb_register(&mic_cb);
}
