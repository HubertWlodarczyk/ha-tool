/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Hearing aid services of the simulated hearing aid (servers): volume renderer with VOCS
 * (balance), presets (HAS) and microphone (MICP with AICS). Their changes are reported as events.
 */

#include <errno.h>
#include <stdlib.h>

#include <zephyr/bluetooth/audio/aics.h>
#include <zephyr/bluetooth/audio/has.h>
#include <zephyr/bluetooth/audio/micp.h>
#include <zephyr/bluetooth/audio/vocs.h>
#include <zephyr/bluetooth/audio/vcp.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include "cli.h"
#include "dut.h"

/* --- Volume renderer --- */

static void vcp_state(struct bt_conn *conn, int err, uint8_t volume, uint8_t mute)
{
	ARG_UNUSED(conn);

	if (err == 0) {
		cli_evt("dut_volume", "volume=%u mute=%u", volume, mute);
	}
}

static struct bt_vcp_vol_rend_cb vcp_cbs = {
	.state = vcp_state,
};

static void vocs_state(struct bt_vocs *inst, int err, int16_t offset)
{
	ARG_UNUSED(inst);

	if (err == 0) {
		cli_evt("dut_balance", "offset=%d", offset);
	}
}

static struct bt_vocs_cb vocs_cbs = {
	.state = vocs_state,
};

/* --- Presets (HAS server) --- */

static const char *const preset_names[] = {"Universal", "Restaurant", "Music"};

static int preset_select(uint8_t index, bool sync)
{
	ARG_UNUSED(sync);

	if (index == 0 || index > ARRAY_SIZE(preset_names)) {
		return -ENOENT;
	}

	cli_evt("dut_preset", "active=%u name=%s", index, preset_names[index - 1]);

	return bt_has_preset_active_set(index);
}

static const struct bt_has_preset_ops preset_ops = {
	.select = preset_select,
};

/* --- Microphone (MICP + AICS servers) --- */

static void mic_mute(uint8_t mute)
{
	cli_evt("dut_mic", "mute=%u", mute);
}

static struct bt_micp_mic_dev_cb mic_cbs = {
	.mute = mic_mute,
};

static void mic_aics_state(struct bt_aics *inst, int err, int8_t gain, uint8_t mute, uint8_t mode)
{
	ARG_UNUSED(inst);

	if (err == 0) {
		cli_evt("dut_mic_input", "gain=%d mute=%u mode=%u", gain, mute, mode);
	}
}

static struct bt_aics_cb mic_aics_cbs = {
	.state = mic_aics_state,
};

static int register_has_micp(void)
{
	const struct bt_has_features_param features = {
		.type = BT_HAS_HEARING_AID_TYPE_MONAURAL,
		.preset_sync_support = false,
		.independent_presets = false,
	};
	const struct bt_aics_register_param mic_aics = {
		.gain = 0,
		.mute = BT_AICS_STATE_UNMUTED,
		.gain_mode = BT_AICS_MODE_MANUAL,
		.units = 1,
		.min_gain = -60,
		.max_gain = 30,
		.type = BT_AICS_INPUT_TYPE_MICROPHONE,
		.status = true,
		.desc_writable = false,
		.description = "Microphone",
		.cb = &mic_aics_cbs,
	};
	struct bt_micp_mic_dev_register_param mic_param = {
		.cb = &mic_cbs,
	};
	int err;

	err = bt_has_register(&features);
	for (size_t i = 0; err == 0 && i < ARRAY_SIZE(preset_names); i++) {
		const struct bt_has_preset_register_param param = {
			.index = i + 1,
			.properties = BT_HAS_PROP_AVAILABLE,
			.name = preset_names[i],
			.ops = &preset_ops,
		};

		err = bt_has_preset_register(&param);
	}

	mic_param.aics_param[0] = mic_aics;
	err = err ?: bt_has_preset_active_set(1);
	err = err ?: bt_micp_mic_dev_register(&mic_param);

	return err;
}

int ha_services_init(void)
{
	const struct bt_vocs_register_param speaker = {
		.location = BT_AUDIO_LOCATION_FRONT_LEFT,
		.location_writable = false,
		.offset = 0,
		.output_desc = "Speaker",
		.desc_writable = false,
		.cb = &vocs_cbs,
	};
	struct bt_vcp_vol_rend_register_param vcp_param = {
		.step = 1,
		.mute = BT_VCP_STATE_UNMUTED,
		.volume = 100,
		.cb = &vcp_cbs,
	};
	int err;

	vcp_param.vocs_param[0] = speaker;
	err = bt_vcp_vol_rend_register(&vcp_param);

	return err != 0 ? err : register_has_micp();
}

/* The hearing aid user turns its volume wheel: the server's change counter moves, which a phone
 * connecting later has to cope with (its first write carries a stale counter)
 */
static int cmd_volume(const struct shell *sh, size_t argc, char **argv)
{
	char *end;
	const long volume = strtol(argv[1], &end, 10);
	int err;

	ARG_UNUSED(argc);

	if (*end != '\0' || volume < 0 || volume > 255) {
		return cli_err(sh, -EINVAL, "volume 0..255");
	}

	err = bt_vcp_vol_rend_set_vol((uint8_t)volume);
	if (err != 0) {
		return cli_err(sh, err, "volume change failed");
	}

	cli_ok(sh, "volume=%ld", volume);

	return 0;
}

SHELL_CMD_ARG_REGISTER(volume, NULL, "Local volume change <0..255> (the user's wheel)", cmd_volume,
		       2, 0);
