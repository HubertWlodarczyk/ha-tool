/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef LEABENCH_COMMON_HCI_UTIL_H_
#define LEABENCH_COMMON_HCI_UTIL_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/conn.h>

/* RSSI of the last packets received on the link (HCI Read RSSI), or 127 if unavailable */
int8_t hci_read_rssi(struct bt_conn *conn);

/* TX power of a connection or an advertising set (and its periodic advertising and BIG) through
 * the Zephyr vendor command; *selected gets the level the radio supports (nearest lower).
 * handle_type: BT_HCI_VS_LL_HANDLE_TYPE_CONN / _ADV.
 */
int hci_set_tx_power(uint8_t handle_type, uint16_t handle, int8_t dbm, int8_t *selected);

struct bt_hci_vs_sched_entry;

/* The controller's running roles (vendor Read Schedule, Zephyr patch 0013): number of entries
 * written to out (at most max), or a negative errno
 */
int hci_read_schedule(struct bt_hci_vs_sched_entry *out, size_t max);

#endif /* LEABENCH_COMMON_HCI_UTIL_H_ */
