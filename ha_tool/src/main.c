/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "version.h"
#include "dev.h"
#include "audio_tx.h"
#include "phone.h"
#include "bcast.h"
#include "cli.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
	bt_addr_le_t addr;
	size_t count = 1;
	char addr_str[CLI_ADDR_STR_LEN];
	int err;

	err = bt_enable(NULL);
	if (err != 0) {
		LOG_ERR("Bluetooth init failed: %d", err);
		cli_evt("fatal", "reason=bt_enable err=%d", err);
		return 0;
	}

	/* No settings_load(): nothing is ever written to flash, every boot starts empty */
	err = dev_init();
	err = err ?: audio_tx_init();
	err = err ?: phone_init();
	err = err ?: bcast_init();
	if (err != 0) {
		LOG_ERR("Module init failed: %d", err);
		cli_evt("fatal", "reason=init err=%d", err);
		return 0;
	}

	bt_id_get(&addr, &count);
	cli_addr_str(&addr, addr_str, sizeof(addr_str));
	LOG_INF("HA-Tool %s ready, identity %s", HA_TOOL_VERSION, addr_str);
	cli_evt("ready", "app=ha_tool version=%s proto=%d addr=%s identities=%u", HA_TOOL_VERSION,
		HA_TOOL_PROTO, addr_str, dev_count());

	return 0;
}
