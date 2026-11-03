/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * dut_sim: a simulated LE Audio device used as the device under test while developing
 * leabench without hardware. CAP acceptor (unicast sink + source, broadcast sink), call control
 * client, media control client and volume renderer, with a small bond list.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/lc3.h>
#include <zephyr/bluetooth/audio/pacs.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/version.h>

#include "cli.h"
#include "hci_util.h"
#include "dut.h"

LOG_MODULE_REGISTER(dut, LOG_LEVEL_INF);

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
	BT_DATA_BYTES(BT_DATA_UUID16_SOME, BT_UUID_16_ENCODE(BT_UUID_ASCS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_CAS_VAL), BT_UUID_16_ENCODE(BT_UUID_VCS_VAL)),
	BT_DATA_BYTES(BT_DATA_SVC_DATA16, BT_UUID_16_ENCODE(BT_UUID_CAS_VAL),
		      BT_AUDIO_UNICAST_ANNOUNCEMENT_GENERAL),
	BT_DATA_BYTES(BT_DATA_SVC_DATA16, BT_UUID_16_ENCODE(BT_UUID_ASCS_VAL),
		      BT_AUDIO_UNICAST_ANNOUNCEMENT_GENERAL, BT_BYTES_LIST_LE16(DUT_SINK_CONTEXTS),
		      BT_BYTES_LIST_LE16(DUT_SOURCE_CONTEXTS), 0x00),
};

static struct bt_le_ext_adv *adv;
static bool adv_enabled = true;
static bool adv_restart; /* the set was ended by a connection */
static bool adv_held;    /* paused while a periodic advertising sync is established */
static struct bt_conn *conns[CONFIG_BT_MAX_CONN];
/* conns[] changes in the Bluetooth RX thread (connected / disconnected) */
static struct k_spinlock conns_lock;

/* A reference to link i for a shell command, NULL if none; the caller unrefs it */
static struct bt_conn *conn_get(size_t i)
{
	struct bt_conn *conn = NULL;

	K_SPINLOCK(&conns_lock) {
		conn = conns[i] != NULL ? bt_conn_ref(conns[i]) : NULL;
	}

	return conn;
}

static void adv_work_handler(struct k_work *work);
static K_WORK_DEFINE(adv_work, adv_work_handler);

static uint8_t conn_count(void)
{
	uint8_t n = 0;

	for (size_t i = 0; i < ARRAY_SIZE(conns); i++) {
		n += conns[i] != NULL;
	}

	return n;
}

static void adv_work_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	if (!adv_enabled || adv_held || conn_count() >= CONFIG_BT_MAX_CONN) {
		(void)bt_le_ext_adv_stop(adv);
		return;
	}

	/* A connection ends the advertising set but the host may still see it as enabled:
	 * stop/start it then. Never stop a running set otherwise (stopping and restarting an
	 * active set right after a disconnection stalls the simulated controller).
	 */
	if (adv_restart) {
		adv_restart = false;
		(void)bt_le_ext_adv_stop(adv);
	}

	err = bt_le_ext_adv_start(adv, BT_LE_EXT_ADV_START_DEFAULT);
	if (err != 0 && err != -EALREADY) {
		LOG_WRN("Advertising start failed: %d", err);
	}
}

void dut_adv_hold(bool hold)
{
	if (hold != adv_held) {
		adv_held = hold;
		k_work_submit(&adv_work);
	}
}

/* --- Connection callbacks --- */

static void connected(struct bt_conn *conn, uint8_t err)
{
	char peer[CLI_ADDR_STR_LEN];

	if (err != 0) {
		return;
	}

	for (size_t i = 0; i < ARRAY_SIZE(conns); i++) {
		bool stored = false;

		K_SPINLOCK(&conns_lock) {
			if (conns[i] == NULL) {
				conns[i] = bt_conn_ref(conn);
				stored = true;
			}
		}

		if (stored) {
			break;
		}
	}

	cli_addr_str(bt_conn_get_dst(conn), peer, sizeof(peer));
	cli_evt("dut_connected", "peer=%s conns=%u bonded=%d", peer, conn_count(),
		bt_le_bond_exists(BT_ID_DEFAULT, bt_conn_get_dst(conn)));
	adv_restart = true;
	k_work_submit(&adv_work);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char peer[CLI_ADDR_STR_LEN];

	for (size_t i = 0; i < ARRAY_SIZE(conns); i++) {
		struct bt_conn *gone = NULL;

		K_SPINLOCK(&conns_lock) {
			if (conns[i] == conn) {
				gone = conns[i];
				conns[i] = NULL;
			}
		}

		if (gone != NULL) {
			bt_conn_unref(gone);
		}
	}

	ctrl_on_disconnected(conn);
	cli_addr_str(bt_conn_get_dst(conn), peer, sizeof(peer));
	cli_evt("dut_disconnected", "peer=%s reason=%s conns=%u", peer, cli_hci_reason_str(reason),
		conn_count());
}

static void recycled(void)
{
	k_work_submit(&adv_work);
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	char peer[CLI_ADDR_STR_LEN];

	cli_addr_str(bt_conn_get_dst(conn), peer, sizeof(peer));
	if (err != BT_SECURITY_ERR_SUCCESS) {
		cli_evt("dut_security_failed", "peer=%s err=%d", peer, err);
		return;
	}

	cli_evt("dut_security", "peer=%s level=%u", peer, level);
	if (level >= BT_SECURITY_L2) {
		ctrl_on_secure(conn);
	}
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
			     uint16_t timeout)
{
	ARG_UNUSED(conn);

	cli_evt("dut_param_updated", "interval_us=%u latency=%u sto_ms=%u", interval * 1250U,
		latency, timeout * 10U);
}

static void le_phy_updated(struct bt_conn *conn, struct bt_conn_le_phy_info *param)
{
	static const char *const names[] = {"none", "1m", "2m", "none", "coded"};

	ARG_UNUSED(conn);

	cli_evt("dut_phy_updated", "tx=%s rx=%s", names[MIN(param->tx_phy, 4)],
		names[MIN(param->rx_phy, 4)]);
}

BT_CONN_CB_DEFINE(conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
	.security_changed = security_changed,
	.le_param_updated = le_param_updated,
	.le_phy_updated = le_phy_updated,
};

/* --- Init --- */

static int register_pacs(void)
{
	static const struct bt_audio_codec_cap sink_cap = BT_AUDIO_CODEC_CAP_LC3(
		BT_AUDIO_CODEC_CAP_FREQ_ANY,
		BT_AUDIO_CODEC_CAP_DURATION_7_5 | BT_AUDIO_CODEC_CAP_DURATION_10,
		BT_AUDIO_CODEC_CAP_CHAN_COUNT_SUPPORT(1), 26, 155, 1, DUT_SINK_CONTEXTS);
	static const struct bt_audio_codec_cap source_cap = BT_AUDIO_CODEC_CAP_LC3(
		BT_AUDIO_CODEC_CAP_FREQ_16KHZ | BT_AUDIO_CODEC_CAP_FREQ_24KHZ |
			BT_AUDIO_CODEC_CAP_FREQ_32KHZ | BT_AUDIO_CODEC_CAP_FREQ_48KHZ,
		BT_AUDIO_CODEC_CAP_DURATION_7_5 | BT_AUDIO_CODEC_CAP_DURATION_10,
		BT_AUDIO_CODEC_CAP_CHAN_COUNT_SUPPORT(1), 26, 155, 1, DUT_SOURCE_CONTEXTS);
	static struct bt_pacs_cap sink = {.codec_cap = &sink_cap};
	static struct bt_pacs_cap source = {.codec_cap = &source_cap};
	const struct bt_pacs_register_param param = {
		.snk_pac = true,
		.snk_loc = true,
		.src_pac = true,
		.src_loc = true,
	};
	int err;

	err = bt_pacs_register(&param);
	err = err ?: bt_pacs_cap_register(BT_AUDIO_DIR_SINK, &sink);
	err = err ?: bt_pacs_cap_register(BT_AUDIO_DIR_SOURCE, &source);
	err = err ?: bt_pacs_set_location(BT_AUDIO_DIR_SINK, BT_AUDIO_LOCATION_FRONT_LEFT);
	err = err ?: bt_pacs_set_location(BT_AUDIO_DIR_SOURCE, BT_AUDIO_LOCATION_FRONT_LEFT);
	if (err != 0) {
		return err;
	}

	(void)bt_pacs_set_supported_contexts(BT_AUDIO_DIR_SINK, DUT_SINK_CONTEXTS);
	(void)bt_pacs_set_available_contexts(BT_AUDIO_DIR_SINK, DUT_SINK_CONTEXTS);
	(void)bt_pacs_set_supported_contexts(BT_AUDIO_DIR_SOURCE, DUT_SOURCE_CONTEXTS);
	(void)bt_pacs_set_available_contexts(BT_AUDIO_DIR_SOURCE, DUT_SOURCE_CONTEXTS);

	return 0;
}

int main(void)
{
	bt_addr_le_t addr;
	size_t count = 1;
	char addr_str[CLI_ADDR_STR_LEN];
	int err;

	err = bt_enable(NULL);
	err = err ?: settings_load();
	err = err ?: pairing_init();
	err = err ?: register_pacs();
	err = err ?: unicast_init();
	err = err ?: ctrl_init();
	err = err ?: audio_services_init();
	err = err ?: bsink_init();
	err = err ?: bt_le_ext_adv_create(BT_BAP_ADV_PARAM_CONN_QUICK, NULL, &adv);
	err = err ?: bt_le_ext_adv_set_data(adv, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err != 0) {
		LOG_ERR("Init failed: %d", err);
		cli_evt("fatal", "err=%d", err);
		return 0;
	}

	k_work_submit(&adv_work);

	bt_id_get(&addr, &count);
	cli_addr_str(&addr, addr_str, sizeof(addr_str));
	cli_evt("ready", "app=dut_sim version=%s addr=%s max_bonds=%d bonds=%d", DUT_VERSION,
		addr_str, CONFIG_BT_MAX_PAIRED, dut_bond_count());

	return 0;
}

/* --- Shell commands --- */

static int cmd_version(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	cli_ok(sh, "app=dut_sim version=%s zephyr=%s", DUT_VERSION, KERNEL_VERSION_STRING);

	return 0;
}

static int cmd_addr(const struct shell *sh, size_t argc, char **argv)
{
	bt_addr_le_t addr;
	size_t count = 1;
	char addr_str[CLI_ADDR_STR_LEN];

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	bt_id_get(&addr, &count);
	cli_addr_str(&addr, addr_str, sizeof(addr_str));
	cli_ok(sh, "addr=%s", addr_str);

	return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	cli_ok(sh, "conns=%u bonds=%d max_bonds=%d adv=%d streams=%u bcast=%s overwrite_oldest=%d",
	       conn_count(), dut_bond_count(), CONFIG_BT_MAX_PAIRED, adv_enabled,
	       unicast_stream_count(), bsink_state_str(),
	       IS_ENABLED(CONFIG_BT_KEYS_OVERWRITE_OLDEST));

	return 0;
}

/* RSSI of every link: what the device receives from the phone(s) */
static int cmd_rssi(const struct shell *sh, size_t argc, char **argv)
{
	char addr[CLI_ADDR_STR_LEN];
	int n = 0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (size_t i = 0; i < ARRAY_SIZE(conns); i++) {
		struct bt_conn *conn = conn_get(i);

		if (conn != NULL) {
			cli_addr_str(bt_conn_get_dst(conn), addr, sizeof(addr));
			cli_line(sh, "LINK peer=%s rssi=%d", addr, hci_read_rssi(conn));
			bt_conn_unref(conn);
			n++;
		}
	}

	cli_ok(sh, "links=%d", n);

	return 0;
}

static int cmd_adv(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0) {
		return cli_err(sh, -EINVAL, "use on|off");
	}

	adv_enabled = strcmp(argv[1], "on") == 0;
	k_work_submit(&adv_work);
	cli_ok(sh, "adv=%d", adv_enabled);

	return 0;
}

static int cmd_disconnect(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (size_t i = 0; i < ARRAY_SIZE(conns); i++) {
		struct bt_conn *conn = conn_get(i);

		if (conn != NULL) {
			(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			bt_conn_unref(conn);
		}
	}

	cli_ok(sh, NULL);

	return 0;
}

/* The device asks the central for other connection parameters (all links) */
static int cmd_conn_param(const struct shell *sh, size_t argc, char **argv)
{
	int err = 0;
	long interval = cli_kv_long(argc, argv, "interval", 50, &err);
	long latency = cli_kv_long(argc, argv, "latency", 0, &err);
	long sto = cli_kv_long(argc, argv, "sto", 4000, &err);
	struct bt_le_conn_param param;
	int sent = 0;

	/* Core spec ranges: interval 7.5 ms..4 s, latency 0..499, timeout 100 ms..32 s */
	if (err != 0 || (interval * 4) % 5 != 0 || sto % 10 != 0 || interval * 4 < 30 ||
	    interval > 4000 || latency < 0 || latency > 499 || sto < 100 || sto > 32000) {
		return cli_err(sh, -EINVAL,
			       "interval=<ms, 1.25 steps> latency=<n> sto=<ms, 10 steps>");
	}

	param.interval_min = param.interval_max = interval * 4 / 5;
	param.latency = latency;
	param.timeout = sto / 10;
	for (size_t i = 0; i < ARRAY_SIZE(conns); i++) {
		struct bt_conn *conn = conn_get(i);

		if (conn != NULL) {
			sent += bt_conn_le_param_update(conn, &param) == 0;
			bt_conn_unref(conn);
		}
	}

	cli_ok(sh, "requested=%d", sent);

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(conn_cmds,
			       SHELL_CMD_ARG(param, NULL,
					     "[interval=<ms>] [latency=<n>] [sto=<ms>]",
					     cmd_conn_param, 1, 3),
			       SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(conn, &conn_cmds, "Link parameters", NULL);

SHELL_CMD_REGISTER(version, NULL, "Firmware version", cmd_version);
SHELL_CMD_REGISTER(addr, NULL, "Identity address", cmd_addr);
SHELL_CMD_REGISTER(status, NULL, "Overall state", cmd_status);
SHELL_CMD_REGISTER(rssi, NULL, "RSSI of every link", cmd_rssi);
SHELL_CMD_ARG_REGISTER(adv, NULL, "on|off", cmd_adv, 2, 0);
SHELL_CMD_REGISTER(disconnect, NULL, "Disconnect all links", cmd_disconnect);
