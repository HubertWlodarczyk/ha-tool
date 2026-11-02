/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * proto human: the events a tester follows in a terminal, as sentences ("Pairing completed",
 * "Passkey required. Continue with: ..."). text / json keep the raw EVT lines for scripts;
 * 'proto human --verbose' prints both.
 */

#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "dev_internal.h"

#define V(_key) (cli_body_get(body, (_key), val, sizeof(val)) ? val : "?")

/* Name of an identity from the id= of an event */
static const char *identity(const char *body)
{
	char id_str[8];
	uint8_t id;

	if (!cli_body_get(body, "id", id_str, sizeof(id_str)) || cli_parse_u8(id_str, &id) != 0) {
		return "?";
	}

	return vdev_valid(id) ? vdevs[id].name : "?";
}

static bool describe_bonding(const char *name, const char *body, char *out, size_t len)
{
	const char *who = identity(body);
	char val[48];

	if (strcmp(name, "bonding_connected") == 0) {
		snprintf(out, len, "Connection established: %s with %s.", who, V("peer"));
	} else if (strcmp(name, "bonding_connect_failed") == 0) {
		snprintf(out, len, "Connection of %s failed (%s).", who, V("reason"));
	} else if (strcmp(name, "bonding_disconnected") == 0) {
		snprintf(out, len, "%s disconnected (%s).", who, V("reason"));
	} else if (strcmp(name, "bonding_security") == 0) {
		snprintf(out, len, "Connection of %s encrypted (level %s).", who, V("level"));
	} else if (strcmp(name, "bonding_security_failed") == 0) {
		snprintf(out, len, "Encryption of %s failed (%s).", who, V("reason"));
	} else if (strcmp(name, "bonding_paired") == 0) {
		snprintf(out, len, "Pairing completed%s for %s.",
			 strcmp(V("bonded"), "1") == 0 ? ", bond stored" : " (no bond)", who);
	} else if (strcmp(name, "bonding_pair_failed") == 0) {
		snprintf(out, len, "Pairing of %s failed (%s).", who, V("reason"));
	} else if (strcmp(name, "bonding_passkey_request") == 0) {
		snprintf(out, len, "Passkey required. Continue with: bonding passkey %s <passkey>",
			 who);
	} else if (strcmp(name, "bonding_confirm") == 0) {
		snprintf(out, len, "Confirm passkey %s for %s: bonding confirm %s yes",
			 V("passkey"), who, who);
	} else if (strcmp(name, "bonding_passkey") == 0) {
		snprintf(out, len, "Passkey %s for %s: enter it on the device.", V("passkey"), who);
	} else {
		return false;
	}

	return true;
}

/* Field values of one event, each in a buffer of its own */
struct fields {
	char a[48];
	char b[48];
	char c[48];
};

static const char *get(const char *body, const char *key, char *buf)
{
	return cli_body_get(body, key, buf, sizeof(((struct fields *)0)->a)) ? buf : "?";
}

static bool describe_source(const char *name, const char *body, char *out, size_t len)
{
	struct fields f;

	if (strcmp(name, "source_state") == 0) {
		snprintf(out, len, "Source: %s -> %s (%s).", get(body, "from", f.a),
			 get(body, "to", f.b), get(body, "cause", f.c));
	} else if (strcmp(name, "source_stream_open") == 0) {
		snprintf(out, len, "Audio stream active: %s, %s, tone %s Hz.",
			 get(body, "ctx", f.a), get(body, "preset", f.b), get(body, "tone", f.c));
	} else if (strcmp(name, "source_stream_closed") == 0) {
		snprintf(out, len, "Audio stream closed.");
	} else if (strcmp(name, "source_music") == 0) {
		snprintf(out, len, "Music %s.", get(body, "state", f.a));
	} else if (strcmp(name, "source_call") == 0) {
		snprintf(out, len, "Call %s %s.", get(body, "idx", f.a), get(body, "state", f.b));
	} else if (strcmp(name, "source_volume") == 0) {
		snprintf(out, len, "Device volume %s%s.", get(body, "volume", f.a),
			 strcmp(get(body, "mute", f.b), "1") == 0 ? " (muted)" : "");
	} else if (strcmp(name, "source_error") == 0) {
		snprintf(out, len, "%s refused (%s) in state %s.", get(body, "op", f.a),
			 get(body, "err", f.b), get(body, "state", f.c));
	} else if (strcmp(name, "source_link_lost") == 0) {
		snprintf(out, len, "Connection of the attached identity lost.");
	} else {
		return false;
	}

	return true;
}

static bool describe_broadcast(const char *name, const char *body, char *out, size_t len)
{
	struct fields f;
	const char *big = get(body, "big", f.a);

	if (strcmp(name, "broadcast_started") == 0) {
		snprintf(out, len, "BIG %s \"%s\" active, %s BIS.", big, get(body, "name", f.b),
			 get(body, "bis", f.c));
	} else if (strcmp(name, "broadcast_stopped") == 0) {
		snprintf(out, len, "BIG %s stopped.", big);
	} else if (strcmp(name, "broadcast_error") == 0) {
		snprintf(out, len, "BIG %s failed: %s.", big, get(body, "reason", f.b));
	} else if (strcmp(name, "broadcast_warning") == 0) {
		snprintf(out, len, "BIG %s warning: %s.", big, get(body, "reason", f.b));
	} else {
		return false;
	}

	return true;
}

bool cli_evt_human(const char *name, const char *body, char *out, size_t len)
{
	char val[16];

	if (strcmp(name, "mode") == 0) {
		snprintf(out, len, "Mode: %s.", V("to"));
		return true;
	}

	return describe_bonding(name, body, out, len) || describe_source(name, body, out, len) ||
	       describe_broadcast(name, body, out, len);
}
