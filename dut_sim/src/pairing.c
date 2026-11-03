/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Security side of the simulated device: bonds (persisted, like a real one), pairing
 * results, and a pairing IO test double that lets tests play the device user (passkey,
 * numeric comparison).
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

static void count_bond(const struct bt_bond_info *info, void *user_data)
{
	(*(int *)user_data)++;
}

int dut_bond_count(void)
{
	int count = 0;

	bt_foreach_bond(BT_ID_DEFAULT, count_bond, &count);

	return count;
}

static struct bt_conn *pair_conn;

static void remember(struct bt_conn *conn)
{
	if (pair_conn != NULL) {
		bt_conn_unref(pair_conn);
	}

	pair_conn = bt_conn_ref(conn);
}

/* The pairing of conn is over: passkey / confirm no longer apply, and the connection object can
 * be recycled after its disconnection
 */
static void forget(struct bt_conn *conn)
{
	if (pair_conn != NULL && pair_conn == conn) {
		bt_conn_unref(pair_conn);
		pair_conn = NULL;
	}
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char peer[CLI_ADDR_STR_LEN];

	cli_addr_str(bt_conn_get_dst(conn), peer, sizeof(peer));
	cli_evt("dut_bonded", "peer=%s bonded=%d bonds=%d", peer, bonded, dut_bond_count());
	forget(conn);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char peer[CLI_ADDR_STR_LEN];

	cli_addr_str(bt_conn_get_dst(conn), peer, sizeof(peer));
	cli_evt("dut_pair_failed", "peer=%s reason=%d", peer, reason);
	forget(conn);
}

static void bond_deleted(uint8_t id, const bt_addr_le_t *peer)
{
	char addr[CLI_ADDR_STR_LEN];

	ARG_UNUSED(id);

	cli_addr_str(peer, addr, sizeof(addr));
	cli_evt("dut_bond_deleted", "peer=%s", addr);
}

static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	remember(conn);
	cli_evt("dut_passkey", "passkey=%06u", passkey);
}

static void auth_passkey_entry(struct bt_conn *conn)
{
	remember(conn);
	cli_evt("dut_passkey_request", NULL);
}

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
	remember(conn);
	cli_evt("dut_confirm", "passkey=%06u", passkey);
}

static void auth_cancel(struct bt_conn *conn)
{
	ARG_UNUSED(conn);
	cli_evt("dut_pair_cancelled", NULL);
}

static const char *const io_names[] = {"none", "display", "yesno", "keyboard", "kbd_display"};

static const struct bt_conn_auth_cb io_cbs[] = {
	[1] = {.passkey_display = auth_passkey_display, .cancel = auth_cancel},
	[2] = {.passkey_display = auth_passkey_display,
	       .passkey_confirm = auth_passkey_confirm,
	       .cancel = auth_cancel},
	[3] = {.passkey_entry = auth_passkey_entry, .cancel = auth_cancel},
	[4] = {.passkey_display = auth_passkey_display,
	       .passkey_entry = auth_passkey_entry,
	       .passkey_confirm = auth_passkey_confirm,
	       .cancel = auth_cancel},
};

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
	.bond_deleted = bond_deleted,
};

int pairing_init(void)
{
	return bt_conn_auth_info_cb_register(&auth_info_cb);
}

/* --- Shell commands --- */

static void print_bond(const struct bt_bond_info *info, void *user_data)
{
	char peer[CLI_ADDR_STR_LEN];

	cli_addr_str(&info->addr, peer, sizeof(peer));
	cli_line(user_data, "BOND peer=%s", peer);
}

static int cmd_bonds(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	bt_foreach_bond(BT_ID_DEFAULT, print_bond, (void *)sh);
	cli_ok(sh, "count=%d max=%d", dut_bond_count(), CONFIG_BT_MAX_PAIRED);

	return 0;
}

static int cmd_unpair(const struct shell *sh, size_t argc, char **argv)
{
	bt_addr_le_t peer;
	int err;

	ARG_UNUSED(argc);

	if (strcmp(argv[1], "all") == 0) {
		err = bt_unpair(BT_ID_DEFAULT, NULL);
	} else if (cli_parse_addr(argv[1], &peer) == 0) {
		err = bt_unpair(BT_ID_DEFAULT, &peer);
	} else {
		return cli_err(sh, -EINVAL, "usage: unpair all|<addr>");
	}

	if (err != 0) {
		return cli_err(sh, err, "unpair failed");
	}

	cli_ok(sh, "bonds=%d", dut_bond_count());

	return 0;
}

static int cmd_io(const struct shell *sh, size_t argc, char **argv)
{
	size_t io;

	ARG_UNUSED(argc);

	for (io = 0; io < ARRAY_SIZE(io_names); io++) {
		if (strcmp(io_names[io], argv[1]) == 0) {
			break;
		}
	}

	if (io == ARRAY_SIZE(io_names)) {
		return cli_err(sh, -EINVAL, "io: none display yesno keyboard kbd_display");
	}

	(void)bt_conn_auth_cb_register(NULL);
	if (io != 0 && bt_conn_auth_cb_register(&io_cbs[io]) != 0) {
		return cli_err(sh, -EIO, "register failed");
	}

	cli_ok(sh, "io=%s", io_names[io]);

	return 0;
}

static int cmd_passkey(const struct shell *sh, size_t argc, char **argv)
{
	char *end;
	unsigned long passkey = strtoul(argv[1], &end, 10);
	int err;

	ARG_UNUSED(argc);

	if (*end != '\0' || passkey > 999999 || pair_conn == NULL) {
		return cli_err(sh, -EINVAL, "passkey 0..999999 while pairing");
	}

	err = bt_conn_auth_passkey_entry(pair_conn, passkey);
	if (err != 0) {
		return cli_err(sh, err, "passkey entry failed");
	}

	cli_ok(sh, NULL);

	return 0;
}

static int cmd_confirm(const struct shell *sh, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);

	if (pair_conn == NULL || (strcmp(argv[1], "yes") != 0 && strcmp(argv[1], "no") != 0)) {
		return cli_err(sh, -EINVAL, "confirm yes|no while pairing");
	}

	err = strcmp(argv[1], "yes") == 0 ? bt_conn_auth_passkey_confirm(pair_conn)
					  : bt_conn_auth_cancel(pair_conn);
	if (err != 0) {
		return cli_err(sh, err, "confirm failed");
	}

	cli_ok(sh, NULL);

	return 0;
}

SHELL_CMD_ARG_REGISTER(io, NULL, "none|display|yesno|keyboard|kbd_display", cmd_io, 2, 0);
SHELL_CMD_ARG_REGISTER(passkey, NULL, "<passkey>", cmd_passkey, 2, 0);
SHELL_CMD_ARG_REGISTER(confirm, NULL, "yes|no", cmd_confirm, 2, 0);
SHELL_CMD_REGISTER(bonds, NULL, "List bonds", cmd_bonds);
SHELL_CMD_ARG_REGISTER(unpair, NULL, "all|<addr>", cmd_unpair, 2, 0);
