/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Virtual devices: each one is a separate local Bluetooth identity (address + keys) that can
 * connect to and bond with the device under test independently of the others. Used to fill a
 * hearing aid's bond list one entry at a time, reconnect vdev_selected entries, forget keys etc.
 *
 * Connecting as a chosen identity relies on bt_conn_le_create_set_id()
 * (patches/zephyr-0001-central-connect-from-identity.patch).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/bluetooth/hci_vs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/shell/shell.h>

#include "dev.h"
#include "dev_internal.h"
#include "mode.h"
#include "phone.h"
#include "cli.h"
#include "hci_util.h"

LOG_MODULE_REGISTER(dev, LOG_LEVEL_INF);

/* Waiting for a disconnection to complete (polled) */
#define DISCONNECT_TIMEOUT_MS 3000
#define POLL_MS               10

const char *const io_names[] = {"none", "display", "yesno", "keyboard", "kbd_display"};
BUILD_ASSERT(ARRAY_SIZE(io_names) == IO_COUNT, "one name per IO capability");
struct vdev vdevs[CONFIG_BT_ID_MAX];

/* Identities are indexed by uint8_t (Zephyr's bt_id type) */
BUILD_ASSERT(ARRAY_SIZE(vdevs) <= UINT8_MAX, "CONFIG_BT_ID_MAX too large");
uint8_t vdev_selected;

/* Pairing callbacks per IO capability, bound on connection (defined below) */
static const struct bt_conn_auth_cb io_cbs[IO_COUNT];

/*
 * Nothing is stored in flash (it would wear out under automated test loops): identities, bond
 * keys and names live in RAM only. After a reset of the tool the table is empty, while the hearing
 * aid keeps its (now orphaned) bonds.
 */

bool vdev_valid(uint8_t id)
{
	return id > BT_ID_DEFAULT && id < ARRAY_SIZE(vdevs) && vdevs[id].state == SLOT_USED;
}

int vdev_of_conn(struct bt_conn *conn)
{
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) != 0 || info.role != BT_CONN_ROLE_CENTRAL) {
		return -ENOENT;
	}

	if (!vdev_valid(info.id) || vdevs[info.id].conn != conn) {
		return -ENOENT;
	}

	return info.id;
}

void vdev_addr_str(uint8_t id, char *buf, size_t len)
{
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);

	bt_id_get(addrs, &count);
	if (id < count) {
		cli_addr_str(&addrs[id], buf, len);
	} else {
		snprintf(buf, len, "none");
	}
}

static void count_bond(const struct bt_bond_info *info, void *user_data)
{
	(*(int *)user_data)++;
}

int vdev_bond_count(uint8_t id)
{
	int count = 0;

	bt_foreach_bond(id, count_bond, &count);

	return count;
}

static const char *sec_err_str(enum bt_security_err err)
{
	switch (err) {
	case BT_SECURITY_ERR_SUCCESS:
		return "success";
	case BT_SECURITY_ERR_AUTH_FAIL:
		return "auth_fail";
	case BT_SECURITY_ERR_PIN_OR_KEY_MISSING:
		return "key_missing";
	case BT_SECURITY_ERR_OOB_NOT_AVAILABLE:
		return "oob_not_available";
	case BT_SECURITY_ERR_AUTH_REQUIREMENT:
		return "auth_requirement";
	case BT_SECURITY_ERR_PAIR_NOT_SUPPORTED:
		return "pair_not_supported";
	case BT_SECURITY_ERR_PAIR_NOT_ALLOWED:
		return "pair_not_allowed";
	case BT_SECURITY_ERR_INVALID_PARAM:
		return "invalid_param";
	case BT_SECURITY_ERR_KEY_REJECTED:
		return "key_rejected";
	default:
		return "unspecified";
	}
}

struct bt_conn *dev_get_conn(uint8_t id)
{
	if (!vdev_valid(id) || !vdevs[id].connected) {
		return NULL;
	}

	return vdevs[id].conn;
}

uint8_t dev_selected(void)
{
	return vdev_selected;
}

int vdev_find(const char *name)
{
	for (uint8_t id = 1; id < ARRAY_SIZE(vdevs); id++) {
		if (vdevs[id].state == SLOT_USED && strcmp(vdevs[id].name, name) == 0) {
			return id;
		}
	}

	return -ENOENT;
}

void dev_state_get(uint8_t id, struct dev_state *st)
{
	struct bt_conn_info info = {0};
	const bool valid = vdev_valid(id);
	const struct vdev *v = valid ? &vdevs[id] : NULL;

	st->name = valid ? v->name : "none";
	st->connection = !valid || v->conn == NULL ? "disconnected"
			 : v->connected            ? "connected"
						   : "connecting";
	st->security = "unencrypted";
	if (valid && v->connected && bt_conn_get_info(v->conn, &info) == 0 &&
	    info.security.level >= BT_SECURITY_L2) {
		st->security = "encrypted";
	}

	st->bonds = valid ? vdev_bond_count(id) : 0;
	st->bond = st->bonds > 0 ? "present" : "absent";
	strcpy(st->peer, "none");
	if (valid && v->conn != NULL) {
		cli_addr_str(bt_conn_get_dst(v->conn), st->peer, sizeof(st->peer));
	}

	st->peer_name = valid && v->peer_name[0] != '\0' ? v->peer_name : "none";
	st->attached = valid && id == vdev_selected;
}

uint8_t dev_count(void)
{
	uint8_t n = 0;

	for (size_t i = 1; i < ARRAY_SIZE(vdevs); i++) {
		n += vdevs[i].state == SLOT_USED;
	}

	return n;
}

uint8_t dev_conn_count(void)
{
	uint8_t n = 0;

	for (size_t i = 1; i < ARRAY_SIZE(vdevs); i++) {
		n += vdevs[i].state == SLOT_USED && vdevs[i].connected;
	}

	return n;
}

static void wait_disconnected(uint8_t id)
{
	for (int t = 0; t < DISCONNECT_TIMEOUT_MS && vdevs[id].conn != NULL; t += POLL_MS) {
		k_sleep(K_MSEC(POLL_MS));
	}
}

int vdev_destroy(uint8_t id)
{
	size_t count;
	int err;

	if (vdevs[id].conn != NULL) {
		(void)bt_conn_disconnect(vdevs[id].conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		wait_disconnected(id);
	}

	(void)bt_unpair(id, NULL);
	err = bt_id_delete(id);
	if (err != 0) {
		return err;
	}

	/* Deleting the last identity shrinks Zephyr's table: the slot is then free (a new
	 * bt_id_create() returns it), bt_id_reset() would fail on it.
	 */
	bt_id_get(NULL, &count);
	vdevs[id].state = id < count ? SLOT_DELETED : SLOT_FREE;
	if (vdev_selected == id) {
		vdev_selected = 0;
	}

	return 0;
}

void dev_disconnect_all(void)
{
	for (uint8_t id = 1; id < ARRAY_SIZE(vdevs); id++) {
		if (vdevs[id].state == SLOT_USED && vdevs[id].conn != NULL) {
			(void)bt_conn_disconnect(vdevs[id].conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			wait_disconnected(id);
		}
	}
}

void dev_reset(void)
{
	for (uint8_t id = 1; id < ARRAY_SIZE(vdevs); id++) {
		if (vdevs[id].state == SLOT_USED) {
			(void)vdev_destroy(id);
		}
	}

	(void)bt_conn_le_create_set_id(BT_ID_DEFAULT);
}

/* --- Bluetooth callbacks --- */

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	struct bt_conn_info info;
	char peer[CLI_ADDR_STR_LEN];
	char local[CLI_ADDR_STR_LEN];

	if (bt_conn_get_info(conn, &info) != 0 || info.role != BT_CONN_ROLE_CENTRAL ||
	    !vdev_valid(info.id) || vdevs[info.id].conn != conn) {
		return;
	}

	cli_addr_str(bt_conn_get_dst(conn), peer, sizeof(peer));

	if (err != 0) {
		cli_evt("bonding_connect_failed", "id=%u peer=%s hci=0x%02x reason=%s", info.id,
			peer, err,
			err == BT_HCI_ERR_UNKNOWN_CONN_ID ? "timeout" : cli_hci_reason_str(err));
		bt_conn_unref(vdevs[info.id].conn);
		vdevs[info.id].conn = NULL;
		return;
	}

	vdevs[info.id].connected = true;
	if (vdevs[info.id].io != IO_NONE) {
		(void)bt_conn_auth_cb_overlay(conn, &io_cbs[vdevs[info.id].io]);
	}

	cli_addr_str(info.le.local, local, sizeof(local));
	cli_evt("bonding_connected", "id=%u peer=%s local=%s bonded=%d", info.id, peer, local,
		bt_le_bond_exists(info.id, bt_conn_get_dst(conn)));
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	int id = vdev_of_conn(conn);

	if (id < 0) {
		return;
	}

	cli_evt("bonding_disconnected", "id=%d hci=0x%02x reason=%s", id, reason,
		cli_hci_reason_str(reason));
	vdevs[id].connected = false;
	bt_conn_unref(vdevs[id].conn);
	vdevs[id].conn = NULL;
}

static void security_changed_cb(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	int id = vdev_of_conn(conn);

	if (id < 0) {
		return;
	}

	if (err != BT_SECURITY_ERR_SUCCESS) {
		cli_evt("bonding_security_failed", "id=%d level=%u reason=%s", id, level,
			sec_err_str(err));
	} else {
		cli_evt("bonding_security", "id=%d level=%u", id, level);
	}
}

BT_CONN_CB_DEFINE(dev_conn_cb) = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
	.security_changed = security_changed_cb,
};

bool vdev_accept_param_req = true;

static bool le_param_req_cb(struct bt_conn *conn, struct bt_le_conn_param *param)
{
	int id = vdev_of_conn(conn);

	if (id >= 0) {
		cli_evt("bonding_param_req",
			"id=%d interval_us=%u latency=%u sto_ms=%u accepted=%d", id,
			param->interval_max * 1250U, param->latency, param->timeout * 10U,
			vdev_accept_param_req);
	}

	return vdev_accept_param_req;
}

static void le_param_updated_cb(struct bt_conn *conn, uint16_t interval, uint16_t latency,
				uint16_t timeout)
{
	int id = vdev_of_conn(conn);

	if (id >= 0) {
		cli_evt("bonding_param_updated", "id=%d interval_us=%u latency=%u sto_ms=%u", id,
			interval * 1250U, latency, timeout * 10U);
	}
}

const char *vdev_phy_str(uint8_t phy)
{
	switch (phy) {
	case BT_GAP_LE_PHY_1M:
		return "1m";
	case BT_GAP_LE_PHY_2M:
		return "2m";
	case BT_GAP_LE_PHY_CODED:
		return "coded";
	default:
		return "none";
	}
}

static void le_phy_updated_cb(struct bt_conn *conn, struct bt_conn_le_phy_info *param)
{
	int id = vdev_of_conn(conn);

	if (id >= 0) {
		cli_evt("bonding_phy_updated", "id=%d tx=%s rx=%s", id, vdev_phy_str(param->tx_phy),
			vdev_phy_str(param->rx_phy));
	}
}

static void le_data_len_updated_cb(struct bt_conn *conn, struct bt_conn_le_data_len_info *info)
{
	int id = vdev_of_conn(conn);

	if (id >= 0) {
		cli_evt("bonding_dle_updated", "id=%d tx_len=%u rx_len=%u", id, info->tx_max_len,
			info->rx_max_len);
	}
}

BT_CONN_CB_DEFINE(dev_param_cb) = {
	.le_param_req = le_param_req_cb,
	.le_param_updated = le_param_updated_cb,
	.le_phy_updated = le_phy_updated_cb,
	.le_data_len_updated = le_data_len_updated_cb,
};

static void att_mtu_updated_cb(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	int id = vdev_of_conn(conn);

	if (id >= 0) {
		cli_evt("bonding_mtu", "id=%d tx=%u rx=%u", id, tx, rx);
	}
}

static struct bt_gatt_cb gatt_cb = {
	.att_mtu_updated = att_mtu_updated_cb,
};

static void pairing_complete_cb(struct bt_conn *conn, bool bonded)
{
	int id = vdev_of_conn(conn);

	if (id >= 0) {
		cli_evt("bonding_paired", "id=%d bonded=%d", id, bonded);
	}
}

static void pairing_failed_cb(struct bt_conn *conn, enum bt_security_err reason)
{
	int id = vdev_of_conn(conn);

	if (id >= 0) {
		cli_evt("bonding_pair_failed", "id=%d reason=%s", id, sec_err_str(reason));
	}
}

/* Pairing interaction: the tester plays the phone user ('dev passkey', 'dev confirm') */
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	cli_evt("bonding_passkey", "id=%d passkey=%06u", vdev_of_conn(conn), passkey);
}

static void auth_passkey_entry(struct bt_conn *conn)
{
	cli_evt("bonding_passkey_request", "id=%d", vdev_of_conn(conn));
}

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
	cli_evt("bonding_confirm", "id=%d passkey=%06u", vdev_of_conn(conn), passkey);
}

static void auth_cancel(struct bt_conn *conn)
{
	cli_evt("bonding_pair_cancelled", "id=%d", vdev_of_conn(conn));
}

/* The host derives the IO capability from the callbacks that are set */
static const struct bt_conn_auth_cb io_cbs[IO_COUNT] = {
	[IO_DISPLAY] = {.passkey_display = auth_passkey_display, .cancel = auth_cancel},
	[IO_YESNO] = {.passkey_display = auth_passkey_display,
		      .passkey_confirm = auth_passkey_confirm,
		      .cancel = auth_cancel},
	[IO_KEYBOARD] = {.passkey_entry = auth_passkey_entry, .cancel = auth_cancel},
	[IO_KBD_DISPLAY] = {.passkey_display = auth_passkey_display,
			    .passkey_entry = auth_passkey_entry,
			    .passkey_confirm = auth_passkey_confirm,
			    .cancel = auth_cancel},
};
BUILD_ASSERT(ARRAY_SIZE(io_cbs) == IO_COUNT, "callbacks for every IO capability");

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete_cb,
	.pairing_failed = pairing_failed_cb,
};

int dev_init(void)
{
	bt_gatt_cb_register(&gatt_cb);

	return bt_conn_auth_info_cb_register(&auth_info_cb);
}
