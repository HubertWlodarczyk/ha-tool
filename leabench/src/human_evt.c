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
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "dev_internal.h"

#define V(_key) (cli_body_get(body, (_key), val, sizeof(val)) ? val : "?")

/* Value of key in buf (len bytes), "?" if absent */
static const char *get_field(const char *body, const char *key, char *buf, size_t len)
{
	return cli_body_get(body, key, buf, len) ? buf : "?";
}

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
	} else if (strcmp(name, "bonding_pair_cancelled") == 0) {
		snprintf(out, len, "Pairing of %s cancelled.", who);
	} else if (strcmp(name, "bonding_param_updated") == 0) {
		snprintf(out, len,
			 "Connection parameters of %s: interval %s us, latency %s, timeout %s ms.",
			 who, get_field(body, "interval_us", val, 12),
			 get_field(body, "latency", val + 12, 12),
			 get_field(body, "sto_ms", val + 24, 12));
	} else if (strcmp(name, "bonding_param_req") == 0) {
		snprintf(out, len, "The device asks %s for interval %s us (%s).", who,
			 get_field(body, "interval_us", val, 12),
			 strcmp(get_field(body, "accepted", val + 12, 4), "1") == 0 ? "accepted"
										    : "rejected");
	} else if (strcmp(name, "bonding_phy_updated") == 0) {
		snprintf(out, len, "PHY of %s: TX %s, RX %s.", who, get_field(body, "tx", val, 12),
			 get_field(body, "rx", val + 12, 12));
	} else if (strcmp(name, "bonding_dle_updated") == 0) {
		snprintf(out, len, "Data length of %s: TX %s, RX %s octets.", who,
			 get_field(body, "tx_len", val, 12),
			 get_field(body, "rx_len", val + 12, 12));
	} else if (strcmp(name, "bonding_mtu") == 0) {
		out[0] = '\0'; /* low level: --verbose */
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

	if (strcmp(name, "source_state") == 0 || strcmp(name, "source_media") == 0 ||
	    strcmp(name, "source_stream_started") == 0 ||
	    strcmp(name, "source_stream_stopped") == 0) {
		/* Covered by the sentences of source_music / source_stream_open / _closed */
		out[0] = '\0';
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
		const int vol = (int)strtol(get(body, "volume", f.a), NULL, 10);

		snprintf(out, len, "Device volume %d/255 (%d%%)%s.", vol, (vol * 100 + 127) / 255,
			 strcmp(get(body, "mute", f.b), "1") == 0 ? ", muted" : "");
	} else if (strcmp(name, "source_stream_updated") == 0) {
		snprintf(out, len, "Audio stream now %s, tone %s Hz.", get(body, "ctx", f.a),
			 get(body, "tone", f.b));
	} else if (strcmp(name, "source_discovered") == 0) {
		snprintf(out, len, "Services of %s found:%s%s%s%s%s%s.", identity(body),
			 strcmp(get(body, "sink", f.a), "1") == 0 ? " audio sink" : "",
			 strcmp(get(body, "source", f.a), "1") == 0 ? ", microphone stream" : "",
			 strcmp(get(body, "vcp", f.a), "1") == 0 ? ", volume (VCP)" : "",
			 strcmp(get(body, "vocs", f.a), "1") == 0 ? ", balance (VOCS)" : "",
			 strcmp(get(body, "has", f.a), "1") == 0 ? ", presets (HAS)" : "",
			 strcmp(get(body, "micp", f.a), "1") == 0 ? ", microphone control (MICP)"
								  : "");
	} else if (strcmp(name, "source_has") == 0) {
		snprintf(out, len, "Device type: %s.", get(body, "type", f.a));
	} else if (strcmp(name, "source_preset") == 0) {
		snprintf(out, len, "Active preset %s.", get(body, "active", f.a));
	} else if (strcmp(name, "source_preset_list") == 0) {
		snprintf(out, len, "%s presets:", get(body, "count", f.a));
	} else if (strcmp(name, "source_preset_record") == 0) {
		snprintf(out, len, "  %s: %s%s", get(body, "idx", f.a), get(body, "name", f.b),
			 strcmp(get(body, "available", f.c), "1") == 0 ? "" : " (unavailable)");
	} else if (strcmp(name, "source_balance") == 0) {
		snprintf(out, len, "Balance offset %s.", get(body, "offset", f.a));
	} else if (strcmp(name, "source_mic") == 0) {
		snprintf(out, len, "Microphone %s.",
			 strcmp(get(body, "mute", f.a), "1") == 0 ? "muted" : "unmuted");
	} else if (strcmp(name, "source_mic_input") == 0) {
		snprintf(out, len, "Microphone gain %s%s.", get(body, "gain", f.a),
			 strcmp(get(body, "mute", f.b), "1") == 0 ? ", muted" : "");
	} else if (strcmp(name, "source_call_request") == 0) {
		snprintf(out, len, "The device dials %s.", get(body, "uri", f.a));
	} else if (strcmp(name, "source_stopped") == 0) {
		snprintf(out, len, "Source stopped.");
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

	if (strcmp(name, "dropped") == 0) {
		snprintf(out, len, "%s events dropped (the terminal reads too slowly).", V("n"));
		return true;
	}

	if (strcmp(name, "fatal") == 0) {
		snprintf(out, len, "Fatal error: %s (%s).", V("reason"), V("err"));
		return true;
	}

	return describe_bonding(name, body, out, len) || describe_source(name, body, out, len) ||
	       describe_broadcast(name, body, out, len);
}
