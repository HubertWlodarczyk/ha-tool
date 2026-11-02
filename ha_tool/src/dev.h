/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Virtual devices: separate local identities (address, IRK, keys) that connect to and bond
 * with the hearing aid independently (dev.c, commands in dev_cmds.c).
 */

#ifndef HA_TOOL_DEV_H_
#define HA_TOOL_DEV_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/conn.h>

#include "cli.h"

int dev_init(void);

/* Disconnects and deletes all virtual devices */
void dev_reset(void);

/* Disconnects all virtual devices, keeping their identities and bonds */
void dev_disconnect_all(void);

/* Connection of a virtual device, or NULL. No reference is taken. */
struct bt_conn *dev_get_conn(uint8_t id);

/* Identity attached to the LE Audio source ('le-source attach', 0: none) */
uint8_t dev_selected(void);

/* Layers of an identity's state, as 'bonding list', 'status' and 'le-source status' show them */
struct dev_state {
	const char *name;       /* "none" if there is no such identity */
	const char *connection; /* disconnected | connecting | connected */
	const char *security;   /* unencrypted | encrypted */
	const char *bond;       /* absent | present */
	int bonds;
	char peer[CLI_ADDR_STR_LEN]; /* connected device, or "none" */
	const char *peer_name;       /* its Bluetooth name if known (connect by name), or "none" */
	bool attached;
};

void dev_state_get(uint8_t id, struct dev_state *st);
uint8_t dev_count(void);
uint8_t dev_conn_count(void);

#endif /* HA_TOOL_DEV_H_ */
