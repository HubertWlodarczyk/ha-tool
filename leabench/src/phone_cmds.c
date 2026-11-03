/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * 'le-source' commands: validate the arguments and queue a request for the source state machine
 * (phone.c). They never touch its state themselves; the outcome is reported as EVT lines.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/shell/shell.h>

#include "cli.h"
#include "cli_cmds.h"
#include "dev.h"
#include "dev_internal.h"
#include "mode.h"
#include "phone.h"
#include "presets.h"

static int queue(const struct shell *sh, const struct phone_msg *msg)
{
	if (phone_request(msg) != 0) {
		return cli_err(sh, -EBUSY, "source busy");
	}

	cli_ok(sh, "queued=1");

	return 0;
}

/* The attached identity has a connection (ERR with the next step otherwise) */
static int need_attached(const struct shell *sh)
{
	const uint8_t id = dev_selected();
	char hint[NAME_LEN + 40];

	if (id == 0) {
		return cli_err_hint(sh, -ENOTCONN, "le-source attach <identity>",
				    "no identity attached to le-source");
	}

	if (dev_get_conn(id) == NULL) {
		snprintf(hint, sizeof(hint), "bonding connect %s address=<addr>", vdevs[id].name);
		return cli_err_hint(sh, -ENOTCONN, hint, "attached identity %s is not connected",
				    CLI_Q(vdevs[id].name));
	}

	return 0;
}

/*
 * Sends a parsed request (so bad arguments never change the mode): unicast mode, entered from
 * none, and a connected attached identity if the request goes to the device.
 */
static int submit(const struct shell *sh, const struct phone_msg *msg, bool to_device)
{
	const char *refused;

	if (mode_require(sh, MODE_UNICAST) != 0) {
		return -EBUSY;
	}

	if (to_device && need_attached(sh) != 0) {
		return -ENOTCONN;
	}

	refused = phone_refused(msg->ev);
	if (refused != NULL) {
		return cli_err_hint(sh, -EPERM, "le-source status",
				    "not possible while the source is %s", refused);
	}

	return queue(sh, msg);
}

static int get_freq(const struct shell *sh, size_t argc, char **argv, const char *key, long def,
		    uint16_t *out)
{
	int err = 0;
	long v = cli_kv_long(argc, argv, key, def, &err);

	/* The nearest tone of the negotiated codec configuration is sent (lc3_frames.c) */
	if (err != 0 || v < 0 || (v > 0 && v < 100) || v > 8000) {
		return cli_err(sh, -EINVAL, "%s must be 0 or 100..8000 Hz", key);
	}

	*out = v;

	return 0;
}

/* The longest preset name ("441_1_hr") fits the request */
BUILD_ASSERT(sizeof("441_1_hr") <= PHONE_PRESET_LEN, "raise PHONE_PRESET_LEN");

static int get_preset(const struct shell *sh, size_t argc, char **argv, const char *key, char *out)
{
	const char *preset = cli_kv(argc, argv, key);

	if (preset == NULL) {
		out[0] = '\0';
		return 0;
	}

	if (preset_find(PRESET_UNICAST, preset, NULL) == NULL) {
		return cli_err(sh, -EINVAL, "unknown %s (%s)", key, preset_names());
	}

	strncpy(out, preset, PHONE_PRESET_LEN - 1);

	return 0;
}

/* Text argument into a request field; ERR if longer than the field */
static int get_str(const struct shell *sh, size_t argc, char **argv, const char *key,
		   const char *def, char *dst, size_t size)
{
	const char *val = cli_kv(argc, argv, key);

	val = val != NULL ? val : def;
	if (strlen(val) >= size) {
		return cli_err(sh, -EINVAL, "%s max %zu characters", key, size - 1);
	}

	memcpy(dst, val, strlen(val) + 1);

	return 0;
}

/* A request carrying the given payload member */
#define PHONE_REQ(_ev, _kind) {.ev = (_ev), .kind = PHONE_PAYLOAD_##_kind}

int cmd_le_source_music_start(const struct shell *sh, size_t argc, char **argv)
{
	struct phone_msg m = PHONE_REQ(PH_REQ_MUSIC_START, MUSIC);

	if (get_freq(sh, argc, argv, "freq", 440, &m.music.freq) != 0 ||
	    get_preset(sh, argc, argv, "preset", m.music.preset) != 0) {
		return -EINVAL;
	}

	return submit(sh, &m, true);
}

/* Arguments shared by incoming and outgoing calls */
static int parse_call(const struct shell *sh, size_t argc, char **argv, const char *def_uri,
		      struct phone_msg *m)
{
	if (get_freq(sh, argc, argv, "ring_freq", 800, &m->call.ring_freq) != 0 ||
	    get_freq(sh, argc, argv, "call_freq", 300, &m->call.call_freq) != 0 ||
	    get_preset(sh, argc, argv, "call_preset", m->call.preset) != 0 ||
	    get_str(sh, argc, argv, "uri", def_uri, m->call.uri, sizeof(m->call.uri)) != 0 ||
	    get_str(sh, argc, argv, "name", "", m->call.name, sizeof(m->call.name)) != 0) {
		return -EINVAL;
	}

	return 0;
}

int cmd_le_source_call_incoming(const struct shell *sh, size_t argc, char **argv)
{
	struct phone_msg m = PHONE_REQ(PH_REQ_CALL_INCOMING, CALL);

	if (parse_call(sh, argc, argv, "tel:123456789", &m) != 0) {
		return -EINVAL;
	}

	return submit(sh, &m, true);
}

int cmd_le_source_call_outgoing(const struct shell *sh, size_t argc, char **argv)
{
	struct phone_msg m = PHONE_REQ(PH_REQ_CALL_OUTGOING, CALL);

	if (parse_call(sh, argc, argv, "tel:987654321", &m) != 0) {
		return -EINVAL;
	}

	return submit(sh, &m, false);
}

int cmd_le_source_alert(const struct shell *sh, size_t argc, char **argv)
{
	struct phone_msg m = PHONE_REQ(PH_REQ_ALERT, ALERT);
	int err = 0;
	long ms;

	if (get_freq(sh, argc, argv, "freq", 1500, &m.alert.freq) != 0) {
		return -EINVAL;
	}

	ms = cli_kv_long(argc, argv, "ms", 1000, &err);
	if (err != 0 || ms < 100 || ms > 30000) {
		return cli_err(sh, -EINVAL, "ms must be 100..30000");
	}

	m.alert.ms = ms;

	return submit(sh, &m, true);
}

int cmd_le_source_volume_set(const struct shell *sh, size_t argc, char **argv)
{
	struct phone_msg m = PHONE_REQ(PH_REQ_VOLUME, CONTROL);
	uint8_t vol;

	ARG_UNUSED(argc);

	if (cli_parse_u8(argv[1], &vol) != 0) {
		return cli_err(sh, -EINVAL, "volume must be 0..255");
	}

	m.control.value = vol;

	return submit(sh, &m, true);
}

static int volume_step_request(const struct shell *sh, int32_t step)
{
	struct phone_msg m = PHONE_REQ(PH_REQ_VOLUME, CONTROL);

	m.control.value = step;

	return submit(sh, &m, true);
}

int cmd_le_source_volume_up(const struct shell *sh, size_t argc, char **argv)
{
	return volume_step_request(sh, PHONE_VOLUME_UP);
}

int cmd_le_source_volume_down(const struct shell *sh, size_t argc, char **argv)
{
	return volume_step_request(sh, PHONE_VOLUME_DOWN);
}

static int on_off(const struct shell *sh, const char *arg, int32_t *out)
{
	if (strcmp(arg, "on") != 0 && strcmp(arg, "off") != 0) {
		return cli_err(sh, -EINVAL, "use on|off");
	}

	*out = strcmp(arg, "on") == 0;

	return 0;
}

int cmd_le_source_mute(const struct shell *sh, size_t argc, char **argv)
{
	struct phone_msg m = PHONE_REQ(PH_REQ_MUTE, CONTROL);

	ARG_UNUSED(argc);

	if (on_off(sh, argv[1], &m.control.value) != 0) {
		return -EINVAL;
	}

	return submit(sh, &m, true);
}

static int preset_request(const struct shell *sh, enum phone_preset_op op, uint8_t index)
{
	struct phone_msg m = PHONE_REQ(PH_REQ_PRESET, PRESET);

	m.preset.op = op;
	m.preset.index = index;

	return submit(sh, &m, true);
}

int cmd_le_source_preset_list(const struct shell *sh, size_t argc, char **argv)
{
	return preset_request(sh, PRESET_OP_LIST, 0);
}

int cmd_le_source_preset_next(const struct shell *sh, size_t argc, char **argv)
{
	return preset_request(sh, PRESET_OP_NEXT, 0);
}

int cmd_le_source_preset_prev(const struct shell *sh, size_t argc, char **argv)
{
	return preset_request(sh, PRESET_OP_PREV, 0);
}

int cmd_le_source_preset_set(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t index;

	ARG_UNUSED(argc);

	if (cli_parse_u8(argv[1], &index) != 0 || index == 0) {
		return cli_err(sh, -EINVAL, "preset index 1..255");
	}

	return preset_request(sh, PRESET_OP_SET, index);
}

int cmd_le_source_mic_mute(const struct shell *sh, size_t argc, char **argv)
{
	struct phone_msg m = PHONE_REQ(PH_REQ_MIC_MUTE, CONTROL);

	ARG_UNUSED(argc);

	if (on_off(sh, argv[1], &m.control.value) != 0) {
		return -EINVAL;
	}

	return submit(sh, &m, true);
}

int cmd_le_source_mic_gain(const struct shell *sh, size_t argc, char **argv)
{
	struct phone_msg m = PHONE_REQ(PH_REQ_MIC_GAIN, CONTROL);
	char *end;
	long gain;

	ARG_UNUSED(argc);

	gain = strtol(argv[1], &end, 10);
	if (*end != '\0' || gain < -128 || gain > 127) {
		return cli_err(sh, -EINVAL, "gain -128..127");
	}

	m.control.value = gain;

	return submit(sh, &m, true);
}

int cmd_le_source_balance_set(const struct shell *sh, size_t argc, char **argv)
{
	struct phone_msg m = PHONE_REQ(PH_REQ_BALANCE, CONTROL);
	char *end;
	long offset;

	offset = strtol(argv[1], &end, 10);
	if (*end != '\0' || offset < -255 || offset > 255) {
		return cli_err(sh, -EINVAL, "offset -255..255");
	}

	m.control.value = offset;

	return submit(sh, &m, true);
}

/* Requests without arguments */
static int simple_request(const struct shell *sh, enum phone_ev ev)
{
	const struct phone_msg m = {.ev = ev};

	return submit(sh, &m, false);
}

int cmd_le_source_music_stop(const struct shell *sh, size_t argc, char **argv)
{
	return simple_request(sh, PH_REQ_MUSIC_STOP);
}

int cmd_le_source_call_accept(const struct shell *sh, size_t argc, char **argv)
{
	return simple_request(sh, PH_REQ_CALL_ACCEPT);
}

int cmd_le_source_call_remote_answer(const struct shell *sh, size_t argc, char **argv)
{
	return simple_request(sh, PH_REQ_CALL_REMOTE_ANSWER);
}

int cmd_le_source_call_end(const struct shell *sh, size_t argc, char **argv)
{
	return simple_request(sh, PH_REQ_CALL_END);
}

int cmd_le_source_call_hold(const struct shell *sh, size_t argc, char **argv)
{
	return simple_request(sh, PH_REQ_CALL_HOLD);
}

int cmd_le_source_call_retrieve(const struct shell *sh, size_t argc, char **argv)
{
	return simple_request(sh, PH_REQ_CALL_RETRIEVE);
}

int cmd_le_source_stop(const struct shell *sh, size_t argc, char **argv)
{
	return simple_request(sh, PH_REQ_STOP);
}

/* le-source attach <identity>: the identity the source uses (one at a time) */
int cmd_le_source_attach(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t id;

	ARG_UNUSED(argc);

	if (parse_ref(sh, argv[1], &id) != 0) {
		return -ENOENT;
	}

	if (mode_require(sh, MODE_UNICAST) != 0) {
		return -EBUSY;
	}

	if (dev_selected() != 0 && dev_selected() != id) {
		return cli_err_hint(sh, -EBUSY, "le-source detach", "identity %s is attached",
				    CLI_Q(vdevs[dev_selected()].name));
	}

	vdev_selected = id;
	cli_ok(sh, "id=%u name=%s", id, CLI_Q(vdevs[id].name));

	return 0;
}

/* le-source detach: stops the source's audio and calls, then forgets the identity */
int cmd_le_source_detach(const struct shell *sh, size_t argc, char **argv)
{
	const uint8_t id = dev_selected();

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (id == 0) {
		return cli_err_hint(sh, -ENOENT, "le-source attach <identity>",
				    "no identity attached");
	}

	phone_reset();
	vdev_selected = 0;
	cli_ok(sh, "id=%u", id);

	return 0;
}

int cmd_le_source_status(const struct shell *sh, size_t argc, char **argv)
{
	struct phone_status st;
	struct dev_state ds;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	phone_get_status(&st);
	dev_state_get(dev_selected(), &ds);
	if (cli_proto_get() == CLI_PROTO_HUMAN) {
		/* Source, link, stream and call apart (docs/broadcast-ux.md, "Source status") */
		shell_print(sh, "Identity:  %s", ds.name);
		shell_print(sh, "Device:    %s (%s)",
			    strcmp(ds.peer_name, "none") != 0 ? ds.peer_name : ds.peer,
			    ds.connection);
		shell_print(sh, "State:     %s%s", st.state, st.music ? ", music wanted" : "");
		if (st.streaming) {
			shell_print(sh, "Stream:    active, %s, %s%s", audio_ctx_str(st.ctx),
				    st.preset, st.bidir ? ", with microphone" : "");
		} else {
			shell_print(sh, "Stream:    stopped");
		}
		if (st.call_index >= 0) {
			shell_print(sh, "Call:      %d %s", st.call_index, st.call_state);
		}
		if (st.bidir) {
			shell_print(sh, "RX loss:   %u of %u frames", st.rx_lost,
				    st.rx + st.rx_lost);
		}
	}

	cli_ok(sh,
	       "identity=%s device=%s device_name=%s connection=%s state=%s discovered=%d "
	       "stream=%s ctx=%s preset=%s bidir=%d call=%d call_state=%s music=%d tx=%u rx=%u "
	       "rx_lost=%u rx_lost_pct=%u vcp=%d",
	       CLI_Q(ds.name), ds.peer, CLI_Q(ds.peer_name), ds.connection, st.state, st.discovered,
	       st.streaming ? "active" : "stopped", st.streaming ? audio_ctx_str(st.ctx) : "none",
	       st.preset, st.bidir, st.call_index, st.call_state, st.music, st.tx, st.rx,
	       st.rx_lost, st.rx + st.rx_lost > 0 ? st.rx_lost * 100U / (st.rx + st.rx_lost) : 0U,
	       st.vcp);

	return 0;
}
