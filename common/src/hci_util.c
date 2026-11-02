/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include <errno.h>

#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_vs.h>
#include <zephyr/sys/byteorder.h>

#include "hci_util.h"

int8_t hci_read_rssi(struct bt_conn *conn)
{
	struct bt_hci_cp_read_rssi *cp;
	struct bt_hci_rp_read_rssi *rp;
	struct net_buf *buf;
	struct net_buf *rsp = NULL;
	uint16_t handle;
	int8_t rssi = 127;

	if (bt_hci_get_conn_handle(conn, &handle) != 0) {
		return rssi;
	}

	buf = bt_hci_cmd_alloc(K_MSEC(100));
	if (buf == NULL) {
		return rssi;
	}

	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle = sys_cpu_to_le16(handle);
	if (bt_hci_cmd_send_sync(BT_HCI_OP_READ_RSSI, buf, &rsp) == 0) {
		rp = (void *)rsp->data;
		if (rp->status == 0) {
			rssi = rp->rssi;
		}
	}

	if (rsp != NULL) {
		net_buf_unref(rsp);
	}

	return rssi;
}

int hci_set_tx_power(uint8_t handle_type, uint16_t handle, int8_t dbm, int8_t *selected)
{
	struct bt_hci_cp_vs_write_tx_power_level *cp;
	struct bt_hci_rp_vs_write_tx_power_level *rp;
	struct net_buf *buf;
	struct net_buf *rsp = NULL;
	int err;

	buf = bt_hci_cmd_alloc(K_MSEC(100));
	if (buf == NULL) {
		return -ENOBUFS;
	}

	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle_type = handle_type;
	cp->handle = sys_cpu_to_le16(handle);
	cp->tx_power_level = dbm;
	err = bt_hci_cmd_send_sync(BT_HCI_OP_VS_WRITE_TX_POWER_LEVEL, buf, &rsp);
	if (err == 0) {
		rp = (void *)rsp->data;
		err = rp->status == 0 ? 0 : -EIO;
		*selected = rp->selected_tx_power;
	}

	if (rsp != NULL) {
		net_buf_unref(rsp);
	}

	return err;
}
