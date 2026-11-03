/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Device controls:
 *  - Call Control client (TBS client on the phone's GTBS): reports call states, "buttons" to
 *    accept / reject / hold calls
 *  - Media Control client: reports media state, play/pause buttons
 *  - Volume Control renderer: reports volume / mute set by the phone
 */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/audio/aics.h>
#include <zephyr/bluetooth/audio/has.h>
#include <zephyr/bluetooth/audio/mcc.h>
#include <zephyr/bluetooth/audio/micp.h>
#include <zephyr/bluetooth/audio/vocs.h>
#include <zephyr/bluetooth/audio/mcs.h>
#include <zephyr/bluetooth/audio/media_proxy.h>
#include <zephyr/bluetooth/audio/tbs.h>
#include <zephyr/bluetooth/audio/vcp.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include "cli.h"
#include "dut.h"

LOG_MODULE_REGISTER(ctrl, LOG_LEVEL_INF);

#define MAX_CALLS 4

static struct {
	struct bt_conn *conn; /* link of the last call state notification */
	uint8_t inst;
	uint8_t count;
	struct bt_tbs_client_call_state calls[MAX_CALLS];
} call;

/* Zephyr's media control client has a single instance: bind it to one link at a time */
static struct bt_conn *mcc_conn;

/* Control service discovery runs for one link at a time (TBS, then MCS if not yet bound);
 * concurrent discoveries on several links exhaust the ATT resources.
 */
static struct bt_conn *disc_queue[CONFIG_BT_MAX_CONN];
static struct bt_conn *disc_conn;
static bool disc_mcc;

static void disc_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(disc_work, disc_work_handler);

static const char *call_state_str(uint8_t state)
{
	switch (state) {
	case BT_TBS_CALL_STATE_INCOMING:
		return "incoming";
	case BT_TBS_CALL_STATE_DIALING:
		return "dialing";
	case BT_TBS_CALL_STATE_ALERTING:
		return "alerting";
	case BT_TBS_CALL_STATE_ACTIVE:
		return "active";
	case BT_TBS_CALL_STATE_LOCALLY_HELD:
		return "locally_held";
	case BT_TBS_CALL_STATE_REMOTELY_HELD:
		return "remotely_held";
	case BT_TBS_CALL_STATE_LOCALLY_AND_REMOTELY_HELD:
		return "held";
	default:
		return "unknown";
	}
}

static const struct bt_tbs_client_call_state *find_call(uint8_t index)
{
	for (uint8_t i = 0; i < call.count; i++) {
		if (call.calls[i].index == index) {
			return &call.calls[i];
		}
	}

	return NULL;
}

/* --- Call control client --- */

static void disc_finish(struct bt_conn *conn)
{
	if (conn == NULL || conn != disc_conn) {
		return;
	}

	bt_conn_unref(disc_conn);
	disc_conn = NULL;
	disc_mcc = false;
	k_work_reschedule(&disc_work, K_MSEC(100));
}

static void tbs_discover(struct bt_conn *conn, int err, uint8_t tbs_count, bool gtbs_found)
{
	cli_evt("dut_ccp_discovered", "err=%d gtbs=%d tbs=%u", err, gtbs_found, tbs_count);

	/* Calls may have started before we subscribed: read the current state. The server is
	 * known from now on ('call dial' right after this event must not wait for the read).
	 */
	if (err == 0 && gtbs_found) {
		call.conn = conn;
		call.inst = BT_TBS_GTBS_INDEX;
		(void)bt_tbs_client_read_call_state(conn, BT_TBS_GTBS_INDEX);
	}

	if (conn != disc_conn) {
		return;
	}

	if (err == 0 && mcc_conn == NULL) {
		mcc_conn = bt_conn_ref(conn);
		disc_mcc = true;
		k_work_reschedule(&disc_work, K_MSEC(100));
	} else {
		disc_finish(conn);
	}
}

static void tbs_call_state(struct bt_conn *conn, int err, uint8_t inst_index, uint8_t call_count,
			   const struct bt_tbs_client_call_state *call_states)
{
	struct bt_tbs_client_call_state old[MAX_CALLS];
	uint8_t old_count = call.count;

	if (err != 0) {
		return;
	}

	memcpy(old, call.calls, sizeof(old));
	call.conn = conn;
	call.inst = inst_index;
	call.count = MIN(call_count, MAX_CALLS);
	if (call.count > 0) { /* call_states may be NULL without calls */
		memcpy(call.calls, call_states, call.count * sizeof(call_states[0]));
	}

	/* Report new and changed calls ... */
	for (uint8_t i = 0; i < call.count; i++) {
		bool changed = true;

		for (uint8_t j = 0; j < old_count; j++) {
			if (old[j].index == call.calls[i].index) {
				changed = old[j].state != call.calls[i].state;
			}
		}

		if (changed) {
			cli_evt("dut_call", "idx=%u state=%s", call.calls[i].index,
				call_state_str(call.calls[i].state));
		}
	}

	/* ... and calls that are gone */
	for (uint8_t j = 0; j < old_count; j++) {
		if (find_call(old[j].index) == NULL) {
			cli_evt("dut_call", "idx=%u state=ended", old[j].index);
		}
	}
}

static void tbs_cp_result(const char *op, int err, uint8_t call_index)
{
	cli_evt("dut_call_cp", "op=%s idx=%u err=%d", op, call_index, err);
}

#define CP_CB(_name)                                                                               \
	static void tbs_##_name(struct bt_conn *conn, int err, uint8_t inst, uint8_t call_index)   \
	{                                                                                          \
		ARG_UNUSED(conn);                                                                  \
		ARG_UNUSED(inst);                                                                  \
		tbs_cp_result(#_name, err, call_index);                                            \
	}

CP_CB(originate)
CP_CB(terminate)
CP_CB(hold)
CP_CB(accept)
CP_CB(retrieve)

static void tbs_termination_reason(struct bt_conn *conn, int err, uint8_t inst_index,
				   uint8_t call_index, uint8_t reason)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(inst_index);

	if (err == 0) {
		cli_evt("dut_call_terminated", "idx=%u reason=%u", call_index, reason);
	}
}

static struct bt_tbs_client_cb tbs_cbs = {
	.discover = tbs_discover,
	.originate_call = tbs_originate,
	.terminate_call = tbs_terminate,
	.hold_call = tbs_hold,
	.accept_call = tbs_accept,
	.retrieve_call = tbs_retrieve,
	.call_state = tbs_call_state,
	.termination_reason = tbs_termination_reason,
};

/* --- Media control client --- */

static const char *media_state_str(uint8_t state)
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

static void mcc_release(void)
{
	if (mcc_conn != NULL) {
		bt_conn_unref(mcc_conn);
		mcc_conn = NULL;
	}
}

static void mcc_discover_mcs(struct bt_conn *conn, int err)
{
	cli_evt("dut_mcp_discovered", "err=%d", err);
	if (err != 0 && conn == mcc_conn) {
		mcc_release();
	}

	disc_finish(conn);
}

static void mcc_media_state(struct bt_conn *conn, int err, uint8_t state)
{
	ARG_UNUSED(conn);

	if (err == 0) {
		cli_evt("dut_media", "state=%s", media_state_str(state));
	}
}

static void mcc_send_cmd(struct bt_conn *conn, int err, const struct mpl_cmd *cmd)
{
	ARG_UNUSED(conn);

	cli_evt("dut_media_cmd", "opcode=%u err=%d", cmd->opcode, err);
}

static struct bt_mcc_cb mcc_cbs = {
	.discover_mcs = mcc_discover_mcs,
	.read_media_state = mcc_media_state,
	.send_cmd = mcc_send_cmd,
};

static void disc_work_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	if (disc_conn != NULL) {
		if (!disc_mcc) {
			return; /* TBS discovery in progress */
		}

		err = bt_mcc_discover_mcs(disc_conn, true);
		if (err == -EBUSY) {
			k_work_reschedule(&disc_work, K_MSEC(500));
		} else if (err != 0) {
			LOG_WRN("MCS discovery failed: %d", err);
			mcc_release();
			disc_finish(disc_conn);
		} else {
			disc_mcc = false; /* wait for the discovery callback */
		}

		return;
	}

	for (size_t i = 0; i < ARRAY_SIZE(disc_queue); i++) {
		if (disc_queue[i] != NULL) {
			disc_conn = disc_queue[i]; /* reference moves from the queue */
			disc_queue[i] = NULL;
			break;
		}
	}

	if (disc_conn == NULL) {
		return;
	}

	err = bt_tbs_client_discover(disc_conn);
	if (err != 0) {
		LOG_WRN("TBS discovery failed: %d", err);
		disc_finish(disc_conn);
	}
}

/* --- Integration --- */

void ctrl_on_secure(struct bt_conn *conn)
{
	if (conn == disc_conn) {
		return;
	}

	for (size_t i = 0; i < ARRAY_SIZE(disc_queue); i++) {
		if (disc_queue[i] == conn) {
			return;
		}
	}

	for (size_t i = 0; i < ARRAY_SIZE(disc_queue); i++) {
		if (disc_queue[i] == NULL) {
			disc_queue[i] = bt_conn_ref(conn);
			break;
		}
	}

	k_work_reschedule(&disc_work, K_MSEC(200));
}

void ctrl_on_disconnected(struct bt_conn *conn)
{
	if (call.conn == conn) {
		for (uint8_t i = 0; i < call.count; i++) {
			cli_evt("dut_call", "idx=%u state=ended reason=link_lost",
				call.calls[i].index);
		}

		call.conn = NULL;
		call.count = 0;
	}

	for (size_t i = 0; i < ARRAY_SIZE(disc_queue); i++) {
		if (disc_queue[i] == conn) {
			bt_conn_unref(disc_queue[i]);
			disc_queue[i] = NULL;
		}
	}

	disc_finish(conn);

	if (mcc_conn == conn) {
		mcc_release();
	}
}

int ctrl_init(void)
{
	int err;

	err = bt_tbs_client_register_cb(&tbs_cbs);
	err = err ?: bt_mcc_init(&mcc_cbs);

	return err;
}

/* --- Shell commands ("buttons") --- */

/* Picks the call a button press applies to: the given index, or the first in `state` */
static int pick_call(const struct shell *sh, size_t argc, char **argv, int state, uint8_t *idx)
{
	int err = 0;
	long want = cli_kv_long(argc, argv, "idx", -1, &err);

	if (err != 0) {
		return cli_err(sh, -EINVAL, "idx=<call index>");
	}

	if (call.conn == NULL || call.count == 0) {
		return cli_err(sh, -ENOENT, "no call");
	}

	for (uint8_t i = 0; i < call.count; i++) {
		if ((want >= 0 && call.calls[i].index == want) ||
		    (want < 0 && (state < 0 || call.calls[i].state == state))) {
			*idx = call.calls[i].index;
			return 0;
		}
	}

	cli_err(sh, -ENOENT, "no matching call");

	return -ENOENT;
}

static int call_reply(const struct shell *sh, int err, uint8_t idx)
{
	if (err != 0) {
		return cli_err(sh, err, "call control failed");
	}

	cli_ok(sh, "idx=%u", idx);

	return 0;
}

static int cmd_call_accept(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t idx = 0;

	if (pick_call(sh, argc, argv, BT_TBS_CALL_STATE_INCOMING, &idx) != 0) {
		return -ENOENT;
	}

	return call_reply(sh, bt_tbs_client_accept_call(call.conn, call.inst, idx), idx);
}

static int cmd_call_end(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t idx = 0;

	if (pick_call(sh, argc, argv, -1, &idx) != 0) {
		return -ENOENT;
	}

	return call_reply(sh, bt_tbs_client_terminate_call(call.conn, call.inst, idx), idx);
}

static int cmd_call_hold(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t idx = 0;

	if (pick_call(sh, argc, argv, BT_TBS_CALL_STATE_ACTIVE, &idx) != 0) {
		return -ENOENT;
	}

	return call_reply(sh, bt_tbs_client_hold_call(call.conn, call.inst, idx), idx);
}

static int cmd_call_retrieve(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t idx = 0;

	if (pick_call(sh, argc, argv, BT_TBS_CALL_STATE_LOCALLY_HELD, &idx) != 0) {
		return -ENOENT;
	}

	return call_reply(sh, bt_tbs_client_retrieve_call(call.conn, call.inst, idx), idx);
}

static int cmd_call_list(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (uint8_t i = 0; i < call.count; i++) {
		cli_line(sh, "CALL idx=%u state=%s", call.calls[i].index,
			 call_state_str(call.calls[i].state));
	}

	cli_ok(sh, "count=%u", call.count);

	return 0;
}

static int cmd_media(const struct shell *sh, size_t argc, char **argv)
{
	struct mpl_cmd cmd = {.use_param = false};
	int err;

	ARG_UNUSED(argc);

	if (strcmp(argv[1], "play") == 0) {
		cmd.opcode = BT_MCS_OPC_PLAY;
	} else if (strcmp(argv[1], "pause") == 0) {
		cmd.opcode = BT_MCS_OPC_PAUSE;
	} else if (strcmp(argv[1], "next") == 0) {
		cmd.opcode = BT_MCS_OPC_NEXT_TRACK;
	} else {
		return cli_err(sh, -EINVAL, "use play|pause|next");
	}

	if (mcc_conn == NULL) {
		return cli_err(sh, -ENOTCONN, "media control not discovered");
	}

	err = bt_mcc_send_cmd(mcc_conn, &cmd);
	if (err != 0) {
		return cli_err(sh, err, "media command failed");
	}

	cli_ok(sh, "opcode=%u", cmd.opcode);

	return 0;
}

/* The device user dials (CCP originate): the phone reports phone_call_request */
static int cmd_call_dial(const struct shell *sh, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);

	if (call.conn == NULL) {
		return cli_err(sh, -ENOTCONN, "no call control server discovered");
	}

	err = bt_tbs_client_originate_call(call.conn, call.inst, argv[1]);
	if (err != 0) {
		return cli_err(sh, err, "dial failed");
	}

	cli_ok(sh, NULL);

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	call_cmds, SHELL_CMD_ARG(dial, NULL, "<uri> - call from the device", cmd_call_dial, 2, 0),
	SHELL_CMD_ARG(accept, NULL, "[idx=<n>] - answer the incoming call", cmd_call_accept, 1, 1),
	SHELL_CMD_ARG(end, NULL, "[idx=<n>] - reject / hang up", cmd_call_end, 1, 1),
	SHELL_CMD_ARG(hold, NULL, "[idx=<n>]", cmd_call_hold, 1, 1),
	SHELL_CMD_ARG(retrieve, NULL, "[idx=<n>]", cmd_call_retrieve, 1, 1),
	SHELL_CMD_ARG(list, NULL, "Known calls", cmd_call_list, 1, 0), SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(call, &call_cmds, "Call control buttons", NULL);
SHELL_CMD_ARG_REGISTER(media, NULL, "play|pause|next", cmd_media, 2, 0);
