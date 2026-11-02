/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Call signalling as a Telephone Bearer Service server (Generic TBS, Call Control Profile).
 * Local actions are plain wrappers; what the hearing aid does (CCP client) is forwarded to the
 * phone state machine.
 */

#include <errno.h>

#include <zephyr/bluetooth/audio/tbs.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>

#include "cli.h"
#include "phone.h"

LOG_MODULE_REGISTER(call, LOG_LEVEL_INF);

#define LOCAL_URI "tel:000000"

int call_incoming(const char *uri, const char *name)
{
	return bt_tbs_remote_incoming(BT_TBS_GTBS_INDEX, LOCAL_URI, uri,
				      name != NULL && name[0] != '\0' ? name : NULL);
}

int call_originate(const char *uri)
{
	uint8_t idx;
	int err = bt_tbs_originate(BT_TBS_GTBS_INDEX, (char *)uri, &idx);

	return err != 0 ? err : idx;
}

int call_accept(uint8_t idx)
{
	return bt_tbs_accept(idx);
}

int call_remote_answer(uint8_t idx)
{
	return bt_tbs_remote_answer(idx);
}

int call_terminate(uint8_t idx)
{
	return bt_tbs_terminate(idx);
}

int call_hold(uint8_t idx)
{
	return bt_tbs_hold(idx);
}

int call_retrieve(uint8_t idx)
{
	return bt_tbs_retrieve(idx);
}

/* --- Requests from the hearing aid --- */

static bool tbs_originate(struct bt_conn *conn, uint8_t call_index, const char *uri)
{
	ARG_UNUSED(conn);

	cli_evt("source_call_request", "idx=%u op=originate uri=%s", call_index, CLI_Q(uri));
	if (!phone_takes_peer_call()) {
		/* Refused in GTBS, not created and left alerting */
		return false;
	}

	phone_post_event(PH_EV_PEER_ORIGINATED, call_index);

	return true;
}

static void tbs_terminate(struct bt_conn *conn, uint8_t call_index, uint8_t reason)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(reason);

	phone_post_event(PH_EV_PEER_TERMINATED, call_index);
}

static void tbs_accept(struct bt_conn *conn, uint8_t call_index)
{
	ARG_UNUSED(conn);

	phone_post_event(PH_EV_PEER_ACCEPTED, call_index);
}

static void tbs_hold(struct bt_conn *conn, uint8_t call_index)
{
	ARG_UNUSED(conn);

	phone_post_event(PH_EV_PEER_HELD, call_index);
}

static void tbs_retrieve(struct bt_conn *conn, uint8_t call_index)
{
	ARG_UNUSED(conn);

	phone_post_event(PH_EV_PEER_RETRIEVED, call_index);
}

static bool tbs_authorize(struct bt_conn *conn)
{
	ARG_UNUSED(conn);

	return true;
}

static struct bt_tbs_cb tbs_cbs = {
	.originate_call = tbs_originate,
	.terminate_call = tbs_terminate,
	.accept_call = tbs_accept,
	.hold_call = tbs_hold,
	.retrieve_call = tbs_retrieve,
	.authorize = tbs_authorize,
};

int call_init(void)
{
	const struct bt_tbs_register_param gtbs_param = {
		.provider_name = "HA-Tool Phone",
		.uci = "un000",
		.uri_schemes_supported = "tel",
		.gtbs = true,
		.authorization_required = false,
		.technology = BT_BEARER_TECH_4G,
		.optional_opcodes = BT_TBS_OPTIONAL_OPCODE_HOLD | BT_TBS_OPTIONAL_OPCODE_JOIN,
	};
	int err;

	err = bt_tbs_register_bearer(&gtbs_param);
	if (err < 0) {
		LOG_ERR("GTBS register failed: %d", err);
		return err;
	}

	bt_tbs_register_cb(&tbs_cbs);

	return 0;
}
