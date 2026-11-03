/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Media Control Service server backed by the Zephyr media player. The state the phone asked for
 * is remembered, so a state change that differs from it can only come from the device (MCP
 * client) and is forwarded to the phone state machine.
 */

#include <zephyr/bluetooth/audio/mcs.h>
#include <zephyr/bluetooth/audio/media_proxy.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "cli.h"
#include "phone.h"

LOG_MODULE_REGISTER(media, LOG_LEVEL_INF);

static struct media_player *local_player;
static atomic_t expected_state = ATOMIC_INIT(BT_MCS_MEDIA_STATE_PAUSED);

static void send(uint8_t opcode, uint8_t state)
{
	struct mpl_cmd cmd = {.opcode = opcode, .use_param = false};

	atomic_set(&expected_state, state);
	if (local_player != NULL) {
		(void)media_proxy_ctrl_send_command(local_player, &cmd);
	}
}

void media_play(void)
{
	send(BT_MCS_OPC_PLAY, BT_MCS_MEDIA_STATE_PLAYING);
}

void media_pause(void)
{
	send(BT_MCS_OPC_PAUSE, BT_MCS_MEDIA_STATE_PAUSED);
}

static const char *state_str(uint8_t state)
{
	switch (state) {
	case BT_MCS_MEDIA_STATE_PLAYING:
		return "playing";
	case BT_MCS_MEDIA_STATE_PAUSED:
		return "paused";
	case BT_MCS_MEDIA_STATE_SEEKING:
		return "seeking";
	default:
		return "inactive";
	}
}

static void local_player_instance(struct media_player *player, int err)
{
	if (err == 0) {
		local_player = player;
	}
}

static void media_state_recv(struct media_player *player, int err, uint8_t state)
{
	if (err != 0 || player != local_player) {
		return;
	}

	cli_evt("source_media", "state=%s", state_str(state));

	if (state == atomic_get(&expected_state)) {
		return; /* our own request */
	}

	atomic_set(&expected_state, state);
	if (state == BT_MCS_MEDIA_STATE_PLAYING) {
		phone_post_event(PH_EV_PEER_PLAY, 0);
	} else if (state == BT_MCS_MEDIA_STATE_PAUSED) {
		phone_post_event(PH_EV_PEER_PAUSE, 0);
	}
}

static struct media_proxy_ctrl_cbs media_cbs = {
	.local_player_instance = local_player_instance,
	.media_state_recv = media_state_recv,
};

int media_init(void)
{
	int err = media_proxy_pl_init();

	return err ?: media_proxy_ctrl_register(&media_cbs);
}
