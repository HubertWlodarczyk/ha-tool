/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/* Remote volume of the hearing aid (VCP volume controller) */

#include <errno.h>

#include <zephyr/bluetooth/audio/vcp.h>
#include <zephyr/bluetooth/audio/vocs.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "cli.h"
#include "phone.h"

LOG_MODULE_REGISTER(volume, LOG_LEVEL_INF);

/* GATT discovery of the volume services on the hearing aid */
#define DISCOVERY_TIMEOUT K_SECONDS(10)

static struct bt_vcp_vol_ctlr *vol_ctlr;
static struct bt_vocs *vocs; /* volume offset (balance) of the first output */
static K_SEM_DEFINE(sem_disc, 0, 1);
static int disc_err;

bool volume_available(void)
{
	return vol_ctlr != NULL;
}

void volume_forget(void)
{
	vol_ctlr = NULL;
	vocs = NULL;
}

bool volume_has_balance(void)
{
	return vocs != NULL;
}

int volume_balance(int16_t offset)
{
	return vocs != NULL ? bt_vocs_state_set(vocs, offset) : -ENOTSUP;
}

int volume_discover(struct bt_conn *conn)
{
	struct bt_vcp_vol_ctlr *ctlr = NULL;
	int err;

	k_sem_reset(&sem_disc);
	err = bt_vcp_vol_ctlr_discover(conn, &ctlr);
	if (err == 0) {
		err = k_sem_take(&sem_disc, DISCOVERY_TIMEOUT) != 0 ? -ETIMEDOUT : disc_err;
	}

	vol_ctlr = err == 0 ? ctlr : NULL;
	vocs = NULL;
	if (vol_ctlr != NULL) {
		struct bt_vcp_included included;

		if (bt_vcp_vol_ctlr_included_get(vol_ctlr, &included) == 0 &&
		    included.vocs_cnt > 0) {
			vocs = included.vocs[0];
		}
	}

	return err;
}

int volume_set(uint8_t volume)
{
	return vol_ctlr != NULL ? bt_vcp_vol_ctlr_set_vol(vol_ctlr, volume) : -ENOTSUP;
}

int volume_step(bool up)
{
	if (vol_ctlr == NULL) {
		return -ENOTSUP;
	}

	return up ? bt_vcp_vol_ctlr_vol_up(vol_ctlr) : bt_vcp_vol_ctlr_vol_down(vol_ctlr);
}

int volume_mute(bool mute)
{
	if (vol_ctlr == NULL) {
		return -ENOTSUP;
	}

	return mute ? bt_vcp_vol_ctlr_mute(vol_ctlr) : bt_vcp_vol_ctlr_unmute(vol_ctlr);
}

static void vcp_discover(struct bt_vcp_vol_ctlr *ctlr, int err, uint8_t vocs_count,
			 uint8_t aics_count)
{
	ARG_UNUSED(ctlr);
	ARG_UNUSED(vocs_count);
	ARG_UNUSED(aics_count);

	disc_err = err;
	k_sem_give(&sem_disc);
}

static void vcp_state(struct bt_vcp_vol_ctlr *ctlr, int err, uint8_t volume, uint8_t mute)
{
	ARG_UNUSED(ctlr);

	if (err == 0) {
		cli_evt("source_volume", "id=%u volume=%u mute=%u", unicast_dev_id(), volume, mute);
	}
}

static void vocs_state(struct bt_vocs *inst, int err, int16_t offset)
{
	ARG_UNUSED(inst);

	if (err == 0) {
		cli_evt("source_balance", "offset=%d", offset);
	}
}

static struct bt_vcp_vol_ctlr_cb vcp_cb = {
	.discover = vcp_discover,
	.state = vcp_state,
	.vocs_cb = {.state = vocs_state},
};

int volume_init(void)
{
	return bt_vcp_vol_ctlr_cb_register(&vcp_cb);
}
