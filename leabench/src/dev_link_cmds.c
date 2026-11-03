/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/* 'bonding' commands of the links: scan, connect, pairing, parameters, TX power. */

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
#include "cli_cmds.h"
#include "hci_util.h"

/* interval=<ms> (1.25 ms steps) latency=<events> sto=<ms> (supervision timeout, 10 ms steps) */
static int parse_conn_param(const struct shell *sh, size_t argc, char **argv,
			    const struct bt_le_conn_param *def, struct bt_le_conn_param *out)
{
	int err = 0;
	long interval = cli_kv_long(argc, argv, "interval", -1, &err);
	long latency = cli_kv_long(argc, argv, "latency", def->latency, &err);
	long sto = cli_kv_long(argc, argv, "sto", def->timeout * 10, &err);

	*out = *def;
	if (interval >= 0) {
		if ((interval * 4) % 5 != 0 || interval < 8 || interval > 4000) {
			err = -EINVAL;
		}

		out->interval_min = out->interval_max = interval * 4 / 5;
	}

	if (err != 0 || latency < 0 || latency > 499 || sto < 100 || sto > 32000 || sto % 10 != 0) {
		return cli_err(sh, -EINVAL,
			       "interval=7.5..4000 ms (1.25 ms steps) latency=0..499 "
			       "sto=100..32000 ms (10 ms steps)");
	}

	out->latency = latency;
	out->timeout = sto / 10;

	return 0;
}

/* ERR "not connected" with the command that connects it */
static int not_connected(const struct shell *sh, uint8_t id)
{
	char hint[NAME_LEN + 40];

	snprintf(hint, sizeof(hint), "bonding connect %s address=<addr>", vdevs[id].name);
	return cli_err_hint(sh, -ENOTCONN, hint, "identity %s has no active connection",
			    CLI_Q(vdevs[id].name));
}

static struct bt_conn *connected_dev(const struct shell *sh, const char *arg, uint8_t *id)
{
	if (parse_ref(sh, arg, id) != 0 || mode_require(sh, MODE_UNICAST) != 0) {
		return NULL;
	}

	if (!vdevs[*id].connected) {
		not_connected(sh, *id);
		return NULL;
	}

	return vdevs[*id].conn;
}

/* --- Scanning for Bluetooth names (bonding scan, bonding connect bluetooth-name=) --- */

#define SCAN_HITS_MAX 8

struct scan_hit {
	bt_addr_le_t addr;
	char name[NAME_LEN];
	int8_t rssi;
};

static struct scan_hit hits[SCAN_HITS_MAX];
static size_t hit_count;
static const char *hit_filter; /* only this name, or NULL: every named device */
static K_MUTEX_DEFINE(hits_lock);

static bool ad_name(struct bt_data *data, void *user_data)
{
	char *name = user_data;

	if (data->type == BT_DATA_NAME_COMPLETE || data->type == BT_DATA_NAME_SHORTENED) {
		const size_t n = MIN(data->data_len, NAME_LEN - 1);

		memcpy(name, data->data, n);
		name[n] = '\0';
		return false;
	}

	return true;
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type, struct net_buf_simple *ad)
{
	char name[NAME_LEN] = "";
	size_t i;

	ARG_UNUSED(type);

	bt_data_parse(ad, ad_name, name);
	if (name[0] == '\0' || (hit_filter != NULL && strcmp(name, hit_filter) != 0)) {
		return;
	}

	k_mutex_lock(&hits_lock, K_FOREVER);
	for (i = 0; i < hit_count && !bt_addr_le_eq(&hits[i].addr, addr); i++) {
	}

	if (i < ARRAY_SIZE(hits)) {
		bt_addr_le_copy(&hits[i].addr, addr);
		memcpy(hits[i].name, name, sizeof(name));
		hits[i].rssi = rssi;
		hit_count = MAX(hit_count, i + 1);
	}

	k_mutex_unlock(&hits_lock);
}

/* Scans for ms (blocking the shell, events still flow); hits[] holds the named devices */
static int scan_names(uint32_t ms, const char *filter)
{
	int err;

	hit_count = 0;
	hit_filter = filter;
	err = bt_le_scan_start(BT_LE_SCAN_ACTIVE, scan_cb);
	if (err != 0) {
		return err;
	}

	k_sleep(K_MSEC(ms));

	return bt_le_scan_stop();
}

static void print_hits(const struct shell *sh)
{
	char addr[CLI_ADDR_STR_LEN];

	for (size_t i = 0; i < hit_count; i++) {
		cli_addr_str(&hits[i].addr, addr, sizeof(addr));
		cli_line(sh, "DEVICE addr=%s name=%s rssi=%d", addr, CLI_Q(hits[i].name),
			 hits[i].rssi);
	}
}

/* bonding scan [timeout=<ms>]: Bluetooth devices that advertise a name */
int cmd_bonding_scan(const struct shell *sh, size_t argc, char **argv)
{
	int err = 0;
	long ms = cli_kv_long(argc, argv, "timeout", 2000, &err);

	if (err != 0 || ms < 100 || ms > 30000) {
		return cli_err(sh, -EINVAL, "timeout must be 100..30000 ms");
	}

	if (mode_require(sh, MODE_UNICAST) != 0) {
		return -EBUSY;
	}

	err = scan_names(ms, NULL);
	if (err != 0) {
		return cli_err(sh, err, "scan failed");
	}

	print_hits(sh);
	cli_ok(sh, "count=%u", (unsigned int)hit_count);

	return 0;
}

/*
 * The device named `name`: exactly one must answer within the scan. Several are listed and none
 * is chosen (a name is not unique, an RPA changes): the tester connects by address then.
 */
static int find_by_name(const struct shell *sh, uint8_t id, const char *name, long scan_ms,
			bt_addr_le_t *peer)
{
	char addr[CLI_ADDR_STR_LEN];
	char hint[NAME_LEN + CLI_ADDR_STR_LEN + 40];
	int err = scan_names(scan_ms, name);

	if (err != 0) {
		return cli_err(sh, err, "scan failed");
	}

	if (hit_count == 0) {
		return cli_err_hint(sh, -ENOENT, "bonding scan",
				    "no Bluetooth device named %s found", CLI_QS(name));
	}

	if (hit_count > 1) {
		print_hits(sh);
		cli_addr_str(&hits[0].addr, addr, sizeof(addr));
		snprintf(hint, sizeof(hint), "bonding connect %s address=%s", vdevs[id].name, addr);
		return cli_err_hint(sh, -EEXIST, hint,
				    "%u Bluetooth devices named %s: connection not started",
				    (unsigned int)hit_count, CLI_QS(name));
	}

	bt_addr_le_copy(peer, &hits[0].addr);

	return 0;
}

/* bonding connect <identity> address=<addr>|bluetooth-name=<name> [timeout= interval= ...] */
int cmd_bonding_connect(const struct shell *sh, size_t argc, char **argv)
{
	struct bt_conn_le_create_param create = *BT_CONN_LE_CREATE_CONN;
	const char *addr_arg = cli_kv(argc, argv, "address");
	const char *name = cli_kv(argc, argv, "bluetooth-name");
	struct bt_le_conn_param param;
	char peer_str[CLI_ADDR_STR_LEN];
	bt_addr_le_t peer;
	uint8_t id;
	int err = 0;
	long timeout_ms;
	long scan_ms;

	if (parse_ref(sh, argv[1], &id) != 0) {
		return -ENOENT;
	}

	if ((addr_arg == NULL) == (name == NULL)) {
		return cli_err_hint(sh, -EINVAL, "bonding scan",
				    "give address=<addr> or bluetooth-name=<name>");
	}

	if (addr_arg != NULL && cli_parse_addr(addr_arg, &peer) != 0) {
		return cli_err(sh, -EINVAL, "bad address %s", CLI_QS(addr_arg));
	}

	if (name != NULL && (strlen(name) == 0 || strlen(name) >= NAME_LEN)) {
		return cli_err(sh, -EINVAL, "bluetooth-name: 1..%d characters", NAME_LEN - 1);
	}

	timeout_ms = cli_kv_long(argc, argv, "timeout", 5000, &err);
	scan_ms = cli_kv_long(argc, argv, "scan", 2000, &err);
	if (err != 0 || timeout_ms < 100 || timeout_ms > 60000 || scan_ms < 100 ||
	    scan_ms > 30000) {
		return cli_err(sh, -EINVAL, "timeout=100..60000 ms scan=100..30000 ms");
	}

	if (parse_conn_param(sh, argc, argv, BT_BAP_CONN_PARAM_RELAXED, &param) != 0) {
		return -EINVAL;
	}

	if (mode_require(sh, MODE_UNICAST) != 0) {
		return -EBUSY;
	}

	if (vdevs[id].conn != NULL) {
		return cli_err(sh, -EALREADY, "identity %s already %s", CLI_Q(vdevs[id].name),
			       vdevs[id].connected ? "connected" : "connecting");
	}

	if (name != NULL && find_by_name(sh, id, name, scan_ms, &peer) != 0) {
		return -ENOENT;
	}

	err = bt_conn_le_create_set_id(id);
	if (err != 0) {
		return cli_err(sh, err, "another connection is being initiated");
	}

	create.timeout = timeout_ms / 10;
	err = bt_conn_le_create(&peer, &create, &param, &vdevs[id].conn);
	if (err != 0) {
		vdevs[id].conn = NULL;
		return cli_err(sh, err, "connect failed");
	}

	snprintf(vdevs[id].peer_name, sizeof(vdevs[id].peer_name), "%s", name != NULL ? name : "");
	cli_addr_str(&peer, peer_str, sizeof(peer_str));
	cli_ok(sh, "id=%u state=connecting device=%s device_name=%s", id, peer_str,
	       CLI_Q(name != NULL ? name : "none"));

	return 0;
}

int cmd_bonding_pair(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t id;
	int err = 0;
	long sec;

	if (parse_ref(sh, argv[1], &id) != 0) {
		return -ENOENT;
	}

	if (mode_require(sh, MODE_UNICAST) != 0) {
		return -EBUSY;
	}

	sec = cli_kv_long(argc, argv, "sec", BT_SECURITY_L2, &err);
	if (err != 0 || sec < BT_SECURITY_L2 || sec > BT_SECURITY_L4) {
		return cli_err(sh, -EINVAL, "sec must be 2..4");
	}

	if (!vdevs[id].connected) {
		return not_connected(sh, id);
	}

	/* Authenticated pairing (MITM) needs a way to show or enter a passkey; Zephyr only says
	 * -EINVAL
	 */
	if (sec >= BT_SECURITY_L3 && vdevs[id].io == IO_NONE) {
		char hint[2 * sizeof(struct cli_text) + 48];

		snprintf(hint, sizeof(hint), "bonding pair %s sec=2; bonding io %s kbd_display",
			 CLI_QS(vdevs[id].name), CLI_QS(vdevs[id].name));
		return cli_err_hint(sh, -EINVAL, hint,
				    "sec=%ld needs authenticated pairing, identity %s has io=none "
				    "(io applies from the next connection)",
				    sec, CLI_QS(vdevs[id].name));
	}

	err = bt_conn_set_security(vdevs[id].conn, (bt_security_t)sec);
	if (err != 0) {
		return cli_err(sh, err, "pairing of %s could not start", CLI_QS(vdevs[id].name));
	}

	cli_ok(sh, "id=%u sec=%ld", id, sec);

	return 0;
}

int cmd_bonding_disconnect(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t id;
	int err;

	if (parse_ref(sh, argv[1], &id) != 0) {
		return -ENOENT;
	}

	if (mode_require(sh, MODE_UNICAST) != 0) {
		return -EBUSY;
	}

	if (vdevs[id].conn == NULL) {
		return not_connected(sh, id);
	}

	err = bt_conn_disconnect(vdevs[id].conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	if (err != 0) {
		return cli_err(sh, err, "disconnect failed");
	}

	cli_ok(sh, "id=%u", id);

	return 0;
}

int cmd_bonding_passkey(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long passkey;
	char *end;
	uint8_t id;
	int err;

	if (parse_ref(sh, argv[1], &id) != 0) {
		return -ENOENT;
	}

	if (mode_require(sh, MODE_UNICAST) != 0) {
		return -EBUSY;
	}

	passkey = strtoul(argv[2], &end, 10);
	if (*end != '\0' || passkey > 999999) {
		return cli_err(sh, -EINVAL, "passkey: 0..999999");
	}

	if (!vdevs[id].connected) {
		return not_connected(sh, id);
	}

	err = bt_conn_auth_passkey_entry(vdevs[id].conn, passkey);
	if (err != 0) {
		return cli_err(sh, err, "passkey entry failed");
	}

	cli_ok(sh, "id=%u", id);

	return 0;
}

int cmd_bonding_confirm(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t id;
	int err;

	if (parse_ref(sh, argv[1], &id) != 0) {
		return -ENOENT;
	}

	if (mode_require(sh, MODE_UNICAST) != 0) {
		return -EBUSY;
	}

	if (strcmp(argv[2], "yes") != 0 && strcmp(argv[2], "no") != 0) {
		return cli_err(sh, -EINVAL, "use yes|no");
	}

	if (!vdevs[id].connected) {
		return not_connected(sh, id);
	}

	err = strcmp(argv[2], "yes") == 0 ? bt_conn_auth_passkey_confirm(vdevs[id].conn)
					  : bt_conn_auth_cancel(vdevs[id].conn);
	if (err != 0) {
		return cli_err(sh, err, "confirm failed");
	}

	cli_ok(sh, "id=%u", id);

	return 0;
}

int cmd_bonding_param(const struct shell *sh, size_t argc, char **argv)
{
	struct bt_le_conn_param param;
	struct bt_conn_info info;
	struct bt_conn *conn;
	uint8_t id;
	int err;

	conn = connected_dev(sh, argv[1], &id);
	if (conn == NULL) {
		return -EINVAL;
	}

	(void)bt_conn_get_info(conn, &info);
	struct bt_le_conn_param cur = {
		.interval_min = info.le.interval_us / 1250U,
		.interval_max = info.le.interval_us / 1250U,
		.latency = info.le.latency,
		.timeout = info.le.timeout,
	};

	if (parse_conn_param(sh, argc, argv, &cur, &param) != 0) {
		return -EINVAL;
	}

	err = bt_conn_le_param_update(conn, &param);
	if (err != 0) {
		return cli_err(sh, err, "parameter update failed");
	}

	cli_ok(sh, "id=%u", id);

	return 0;
}

int cmd_bonding_phy(const struct shell *sh, size_t argc, char **argv)
{
	/*
	 * Not BT_CONN_LE_PHY_PARAM_*: those are compound literals that end with the block they
	 * are written in, so a pointer kept past the if/else dangles (ASan stack-use-after-scope).
	 */
	static const struct {
		const char *name;
		struct bt_conn_le_phy_param param;
	} phys[] = {
		{"1m", BT_CONN_LE_PHY_PARAM_INIT(BT_GAP_LE_PHY_1M, BT_GAP_LE_PHY_1M)},
		{"2m", BT_CONN_LE_PHY_PARAM_INIT(BT_GAP_LE_PHY_2M, BT_GAP_LE_PHY_2M)},
		{"coded", BT_CONN_LE_PHY_PARAM_INIT(BT_GAP_LE_PHY_CODED, BT_GAP_LE_PHY_CODED)},
	};
	const struct bt_conn_le_phy_param *phy = NULL;
	struct bt_conn *conn;
	uint8_t id;
	int err;

	for (size_t i = 0; i < ARRAY_SIZE(phys); i++) {
		if (strcmp(argv[2], phys[i].name) == 0) {
			phy = &phys[i].param;
		}
	}

	if (phy == NULL) {
		return cli_err(sh, -EINVAL, "phy: 1m 2m coded");
	}

	conn = connected_dev(sh, argv[1], &id);
	if (conn == NULL) {
		return -EINVAL;
	}

	err = bt_conn_le_phy_update(conn, phy);
	if (err != 0) {
		return cli_err(sh, err, "PHY update failed");
	}

	cli_ok(sh, "id=%u", id);

	return 0;
}

int cmd_bonding_dle(const struct shell *sh, size_t argc, char **argv)
{
	struct bt_conn_le_data_len_param param;
	struct bt_conn *conn;
	unsigned long len;
	char *end;
	uint8_t id;
	int err;

	conn = connected_dev(sh, argv[1], &id);
	if (conn == NULL) {
		return -EINVAL;
	}

	len = strtoul(argv[2], &end, 10);
	if (*end != '\0' || len < 27 || len > 251) {
		return cli_err(sh, -EINVAL, "octets: 27..251");
	}

	param.tx_max_len = len;
	param.tx_max_time = BT_GAP_DATA_TIME_MAX;
	err = bt_conn_le_data_len_update(conn, &param);
	if (err != 0) {
		return cli_err(sh, err, "data length update failed");
	}

	cli_ok(sh, "id=%u", id);

	return 0;
}

int cmd_bonding_policy(const struct shell *sh, size_t argc, char **argv)
{
	const char *req = cli_kv(argc, argv, "param_req");

	if (req != NULL) {
		if (strcmp(req, "accept") != 0 && strcmp(req, "reject") != 0) {
			return cli_err(sh, -EINVAL, "param_req=accept|reject");
		}

		vdev_accept_param_req = strcmp(req, "accept") == 0;
	}

	cli_ok(sh, "param_req=%s", vdev_accept_param_req ? "accept" : "reject");

	return 0;
}

/* bonding txpower <identity> <dBm>: TX power of its connection (distance simulation) */
int cmd_bonding_txpower(const struct shell *sh, size_t argc, char **argv)
{
	uint16_t handle;
	int8_t actual;
	char *end;
	long dbm;
	uint8_t id;
	int err;

	if (parse_ref(sh, argv[1], &id) != 0) {
		return -ENOENT;
	}

	if (mode_require(sh, MODE_UNICAST) != 0) {
		return -EBUSY;
	}

	dbm = strtol(argv[2], &end, 10);
	if (*end != '\0' || dbm < -40 || dbm > 8) {
		return cli_err(sh, -EINVAL, "dBm: -40..8");
	}

	if (!vdevs[id].connected || bt_hci_get_conn_handle(vdevs[id].conn, &handle) != 0) {
		return not_connected(sh, id);
	}

	err = hci_set_tx_power(BT_HCI_VS_LL_HANDLE_TYPE_CONN, handle, dbm, &actual);
	if (err != 0) {
		return cli_err(sh, err, "TX power not set");
	}

	cli_ok(sh, "id=%u txpower=%d", id, actual);

	return 0;
}
