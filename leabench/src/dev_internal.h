/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/* Virtual devices internals shared by dev.c (identities, callbacks) and dev_cmds.c (commands). */

#ifndef LEABENCH_DEV_INTERNAL_H_
#define LEABENCH_DEV_INTERNAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/conn.h>

#define NAME_LEN 24

enum slot_state {
	SLOT_FREE, /* never created */
	SLOT_USED,
	SLOT_DELETED, /* identity deleted, can be reclaimed with bt_id_reset() */
};

/* Pairing IO capabilities of a virtual device (as the phone advertises them) */
enum io_cap {
	IO_NONE,
	IO_DISPLAY,
	IO_YESNO, /* display + yes/no: numeric comparison */
	IO_KEYBOARD,
	IO_KBD_DISPLAY,
	IO_COUNT,
};

extern const char *const io_names[]; /* IO_COUNT entries */

struct vdev {
	enum slot_state state;
	char name[NAME_LEN];
	char peer_name[NAME_LEN]; /* Bluetooth name of the device connected by name, or "" */
	uint8_t io;
	struct bt_conn *conn;
	bool connected;
};

/* Indexed by identity; identity 0 is the tool itself */
extern struct vdev vdevs[CONFIG_BT_ID_MAX];

extern uint8_t vdev_selected;
extern bool vdev_accept_param_req;

/* Slot in use (created, not deleted) */
bool vdev_valid(uint8_t id);

/* Device id of a connection, or -1 */
int vdev_of_conn(struct bt_conn *conn);

/* Identity address of a device as text */
void vdev_addr_str(uint8_t id, char *buf, size_t len);

/* Bonds of a device */
int vdev_bond_count(uint8_t id);

/* Disconnects, forgets the keys and deletes the identity of a device */
int vdev_destroy(uint8_t id);

/* "1m", "2m", "coded" */
const char *vdev_phy_str(uint8_t phy);

/* --- dev_cmds.c: argument helpers shared with dev_link_cmds.c --- */

struct shell;

/* Identity given by name or id (ERR printed otherwise) */
int parse_ref(const struct shell *sh, const char *arg, uint8_t *id);

/* Identity of that name, or -ENOENT */
int vdev_find(const char *name);

/* IO capability name: index of io_names[], or -EINVAL */
int parse_io(const char *str);

#endif /* LEABENCH_DEV_INTERNAL_H_ */
