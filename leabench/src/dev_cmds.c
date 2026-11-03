/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/* 'bonding' commands of the identities: create, list, show, unpair, delete, bonds, io. */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "dev.h"
#include "dev_internal.h"
#include "mode.h"
#include "cli.h"
#include "cli_cmds.h"
#include "cli_tree.h"
#include "hci_util.h"

int parse_io(const char *str)
{
	for (int i = 0; i < IO_COUNT; i++) {
		if (strcmp(io_names[i], str) == 0) {
			return i;
		}
	}

	return -EINVAL;
}

static bool all_digits(const char *s)
{
	if (*s == '\0') {
		return false;
	}

	for (; *s != '\0'; s++) {
		if (!isdigit((unsigned char)*s)) {
			return false;
		}
	}

	return true;
}

int parse_ref(const struct shell *sh, const char *arg, uint8_t *id)
{
	int found;

	*id = 0;
	if (all_digits(arg)) {
		if (cli_parse_u8(arg, id) == 0 && vdev_valid(*id)) {
			return 0;
		}
	} else {
		found = vdev_find(arg);
		if (found > 0) {
			*id = found;
			return 0;
		}
	}

	cli_err_hint(sh, -ENOENT, "bonding list", "no identity %s", CLI_QS(arg));

	return -ENOENT;
}

/* bonding create [<name>] [io=] [addr=] */
int cmd_bonding_create(const struct shell *sh, size_t argc, char **argv)
{
	const char *name = argc > 1 && strchr(argv[1], '=') == NULL ? argv[1] : NULL;
	const char *addr_arg = cli_kv(argc, argv, "addr");
	const char *io_arg = cli_kv(argc, argv, "io");
	int io = IO_NONE;
	bt_addr_le_t addr;
	char addr_str[CLI_ADDR_STR_LEN];
	char auto_name[NAME_LEN];
	int id = -ENOMEM;

	if (io_arg != NULL) {
		io = parse_io(io_arg);
		if (io < 0) {
			return cli_err(sh, -EINVAL, "io: none display yesno keyboard kbd_display");
		}
	}

	if (addr_arg != NULL) {
		if (cli_parse_addr(addr_arg, &addr) != 0 || addr.type != BT_ADDR_LE_RANDOM ||
		    !BT_ADDR_IS_STATIC(&addr.a)) {
			return cli_err(sh, -EINVAL, "addr must be a static random address");
		}
	}

	/* A name is the tester's handle: unique, and never a number (that would be an id) */
	if (name != NULL) {
		if (strlen(name) >= NAME_LEN || all_digits(name)) {
			return cli_err(sh, -EINVAL, "name: 1..%d characters, not only digits",
				       NAME_LEN - 1);
		}

		if (vdev_find(name) > 0) {
			return cli_err_hint(sh, -EEXIST, "bonding delete <name>",
					    "identity %s exists", CLI_QS(name));
		}
	}

	if (mode_require(sh, MODE_UNICAST) != 0) {
		return -EBUSY;
	}

	/* Reclaim a deleted identity first, so the identity table does not fill up */
	for (uint8_t i = 1; i < ARRAY_SIZE(vdevs); i++) {
		if (vdevs[i].state == SLOT_DELETED) {
			id = bt_id_reset(i, addr_arg != NULL ? &addr : NULL, NULL);
			break;
		}
	}

	if (id < 0) {
		id = bt_id_create(addr_arg != NULL ? &addr : NULL, NULL);
	}

	if (id < 0) {
		return cli_err(sh, id, "cannot create identity");
	}

	vdevs[id].state = SLOT_USED;
	vdevs[id].conn = NULL;
	vdevs[id].connected = false;
	vdevs[id].peer_name[0] = '\0';
	if (name != NULL) {
		snprintf(vdevs[id].name, sizeof(vdevs[id].name), "%s", name);
	} else {
		/* identity<id>, or identity<id>-2, -3 ... if the tester already took that name */
		vdevs[id].name[0] = '\0';
		snprintf(auto_name, sizeof(auto_name), "identity%u", (uint8_t)id);
		for (unsigned int k = 2; vdev_find(auto_name) > 0; k++) {
			snprintf(auto_name, sizeof(auto_name), "identity%u-%u", (uint8_t)id, k);
		}
		snprintf(vdevs[id].name, sizeof(vdevs[id].name), "%s", auto_name);
	}

	vdevs[id].io = io;

	vdev_addr_str(id, addr_str, sizeof(addr_str));
	cli_ok(sh, "id=%d name=%s addr=%s io=%s", id, CLI_Q(vdevs[id].name), addr_str,
	       io_names[io]);

	return 0;
}

int cmd_bonding_list(const struct shell *sh, size_t argc, char **argv)
{
	const bool human = cli_proto_get() == CLI_PROTO_HUMAN;
	uint8_t n = 0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (human) {
		if (dev_count() == 0) {
			shell_print(sh, "No identities yet. Create one: bonding create <name>");
		} else {
			shell_print(sh, "%-3s %-16s %-13s %-30s %-12s %-8s %s", "ID", "NAME",
				    "CONNECTION", "DEVICE", "SECURITY", "BOND", "SOURCE");
		}
	}

	for (uint8_t id = 1; id < ARRAY_SIZE(vdevs); id++) {
		struct dev_state st;
		char addr[CLI_ADDR_STR_LEN];

		if (vdevs[id].state != SLOT_USED) {
			continue;
		}

		dev_state_get(id, &st);
		vdev_addr_str(id, addr, sizeof(addr));
		if (human) {
			shell_print(sh, "%-3u %-16s %-13s %-30s %-12s %-8s %s", id, st.name,
				    st.connection,
				    strcmp(st.peer_name, "none") != 0 ? st.peer_name : st.peer,
				    st.security, st.bond, st.attached ? "attached" : "no");
		} else {
			cli_line(sh,
				 "IDENTITY id=%u name=%s addr=%s io=%s connection=%s device=%s "
				 "device_name=%s security=%s bond=%s bonds=%d source=%d",
				 id, CLI_Q(st.name), addr, io_names[vdevs[id].io], st.connection,
				 st.peer, CLI_Q(st.peer_name), st.security, st.bond, st.bonds,
				 st.attached);
		}

		n++;
	}

	cli_ok(sh, "count=%u", n);

	return 0;
}

/* bonding show <identity> [--verbose]: one identity; with --verbose the link parameters */
int cmd_bonding_show(const struct shell *sh, size_t argc, char **argv)
{
	struct bt_conn_info info = {0};
	char addr[CLI_ADDR_STR_LEN];
	struct dev_state st;
	char link[160] = "";
	uint8_t id;

	if (parse_ref(sh, argv[1], &id) != 0) {
		return -ENOENT;
	}

	dev_state_get(id, &st);
	vdev_addr_str(id, addr, sizeof(addr));
	if (cli_flag(argc, argv, "verbose")) {
		const bool up = vdevs[id].connected && bt_conn_get_info(vdevs[id].conn, &info) == 0;

		snprintf(link, sizeof(link), " rssi=%d interval_us=%u latency=%u sto_ms=%u phy=%s",
			 up ? hci_read_rssi(vdevs[id].conn) : 127, up ? info.le.interval_us : 0,
			 up ? info.le.latency : 0, up ? info.le.timeout * 10U : 0,
			 up && info.le.phy != NULL ? vdev_phy_str(info.le.phy->tx_phy) : "none");
	}

	cli_ok(sh,
	       "id=%u name=%s addr=%s io=%s connection=%s device=%s device_name=%s security=%s "
	       "bond=%s bonds=%d source=%d%s",
	       id, CLI_Q(st.name), addr, io_names[vdevs[id].io], st.connection, st.peer,
	       CLI_Q(st.peer_name), st.security, st.bond, st.bonds, st.attached, link);

	return 0;
}

int cmd_bonding_unpair(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t id;
	int err;

	ARG_UNUSED(argc);

	if (parse_ref(sh, argv[1], &id) != 0) {
		return -ENOENT;
	}

	if (mode_require(sh, MODE_UNICAST) != 0) {
		return -EBUSY;
	}

	/* Forget the keys locally only: the peer still has them ("phone forgot the device") */
	err = bt_unpair(id, NULL);
	if (err != 0) {
		return cli_err(sh, err, "unpair failed");
	}

	cli_ok(sh, "id=%u bond=absent", id);

	return 0;
}

int cmd_bonding_delete(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t id;
	int err;

	ARG_UNUSED(argc);

	if (parse_ref(sh, argv[1], &id) != 0) {
		return -ENOENT;
	}

	if (mode_require(sh, MODE_UNICAST) != 0) {
		return -EBUSY;
	}

	err = vdev_destroy(id);
	if (err != 0) {
		return cli_err(sh, err, "delete failed");
	}

	cli_ok(sh, "id=%u", id);

	return 0;
}

static void print_bond(const struct bt_bond_info *info, void *user_data)
{
	const struct shell *sh = user_data;
	char peer[CLI_ADDR_STR_LEN];

	cli_addr_str(&info->addr, peer, sizeof(peer));
	cli_line(sh, "BOND peer=%s", peer);
}

int cmd_bonding_bonds(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t id;

	ARG_UNUSED(argc);

	if (parse_ref(sh, argv[1], &id) != 0) {
		return -ENOENT;
	}

	bt_foreach_bond(id, print_bond, (void *)sh);
	cli_ok(sh, "id=%u count=%d", id, vdev_bond_count(id));

	return 0;
}

int cmd_bonding_io(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t id;
	int io;

	ARG_UNUSED(argc);

	if (parse_ref(sh, argv[1], &id) != 0) {
		return -ENOENT;
	}

	io = parse_io(argv[2]);
	if (io < 0) {
		return cli_err(sh, -EINVAL, "io: none display yesno keyboard kbd_display");
	}

	if (mode_require(sh, MODE_UNICAST) != 0) {
		return -EBUSY;
	}

	/* Takes effect on the next connection (callbacks are bound when it is established) */
	vdevs[id].io = io;
	cli_ok(sh, "id=%u io=%s", id, io_names[io]);

	return 0;
}
