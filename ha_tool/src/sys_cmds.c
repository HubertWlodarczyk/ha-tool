/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include <errno.h>
#include <string.h>

#include <stdio.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/version.h>

#include "mode.h"
#include "version.h"
#include "dev.h"
#include "phone.h"
#include "bcast.h"
#include "cli.h"
#include "cli_cmds.h"
#include "cli_tree.h"
#include "lc3_frames.h"

/* Radio mode (mode.h): unicast or broadcast, never both; none after boot and 'reset' */
static enum ha_mode mode = MODE_NONE;

/* The running command entered the mode from none: undone if it fails (see cli_on_reply) */
static bool mode_tentative;

static const char *const mode_names[] = {
	[MODE_NONE] = "none",
	[MODE_UNICAST] = "unicast",
	[MODE_BROADCAST] = "broadcast",
};
BUILD_ASSERT(ARRAY_SIZE(mode_names) == MODE_COUNT, "mode_names out of date");

static const char *mode_name(enum ha_mode m)
{
	return m < MODE_COUNT ? mode_names[m] : "?";
}

const char *mode_str(void)
{
	return mode_name(mode);
}

/* Leaving a mode stops it and clears its whole context: nothing of it is left behind */
static void leave(enum ha_mode next)
{
	if (mode == MODE_UNICAST && next != MODE_UNICAST) {
		phone_reset();
		dev_reset(); /* links, identities and their bonds */
	}

	if (mode == MODE_BROADCAST && next != MODE_BROADCAST) {
		bcast_reset();
	}
}

static void set_mode(enum ha_mode next)
{
	__ASSERT_NO_MSG(next < MODE_COUNT);

	if (next == mode || next >= MODE_COUNT) {
		return;
	}

	leave(next);
	cli_evt("mode", "from=%s to=%s", mode_name(mode), mode_name(next));
	mode = next;
}

int mode_require(const struct shell *sh, enum ha_mode needed)
{
	if (mode == needed) {
		return 0;
	}

	if (mode == MODE_NONE) {
		set_mode(needed);
		mode_tentative = true;
		return 0;
	}

	char hint[24];

	snprintf(hint, sizeof(hint), "mode %s", mode_name(needed));
	cli_err_hint(sh, -EBUSY, hint, "%s mode active; switching clears its whole context",
		     mode_name(mode));

	return -EBUSY;
}

/* A command that failed after entering a mode from none leaves the board as it was */
void cli_on_reply(bool ok)
{
	if (mode_tentative && !ok) {
		set_mode(MODE_NONE);
	}

	mode_tentative = false;
}

int cmd_mode(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		if (strcmp(argv[1], "unicast") == 0) {
			set_mode(MODE_UNICAST);
		} else if (strcmp(argv[1], "broadcast") == 0) {
			set_mode(MODE_BROADCAST);
		} else {
			return cli_err_hint(sh, -EINVAL, "mode unicast; mode broadcast; reset",
					    "mode: unicast or broadcast (reset leaves both)");
		}
	}

	cli_ok(sh, "mode=%s", mode_name(mode));

	return 0;
}

int cmd_version(const struct shell *sh, size_t argc, char **argv)
{

	cli_ok(sh, "app=ha_tool version=%s zephyr=%s proto=%d", HA_TOOL_VERSION,
	       KERNEL_VERSION_STRING, HA_TOOL_PROTO);

	return 0;
}

int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	struct dev_state ds;
	struct cli_stats cs;
	char more[120] = "";
	int active;
	int configured;

	dev_state_get(dev_selected(), &ds);
	bcast_counts(&active, &configured);
	if (cli_flag(argc, argv, "verbose")) {
		cli_stats_get(&cs);
		snprintf(more, sizeof(more),
			 " identities=%u conns=%u lines_truncated=%u fields_dropped=%u "
			 "evt_dropped=%u",
			 dev_count(), dev_conn_count(), cs.lines_truncated, cs.fields_dropped,
			 cs.evt_dropped);
	}

	cli_ok(sh,
	       "mode=%s identity=%s device=%s device_name=%s connection=%s security=%s bond=%s "
	       "source=%s broadcasts_active=%d broadcasts_configured=%d%s",
	       mode_name(mode), CLI_Q(ds.name), ds.peer, CLI_Q(ds.peer_name), ds.connection,
	       ds.security, ds.bond, phone_state_str(), active, configured, more);

	return 0;
}

/* Stops all radio activity of the current mode; identities, bonds and drafts stay */
int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (mode == MODE_UNICAST) {
		phone_reset();
		dev_disconnect_all();
	} else if (mode == MODE_BROADCAST) {
		bcast_stop_all();
	}

	cli_ok(sh, "mode=%s", mode_name(mode));

	return 0;
}

int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	if (cli_proto_get() == CLI_PROTO_HUMAN && !cli_flag(argc, argv, "yes")) {
		return cli_err_hint(sh, -EPERM, "reset --yes",
				    "reset deletes identities, bonds and broadcasts");
	}

	phone_reset();
	bcast_reset();
	dev_reset();
	cli_evt_filter_clear();
	if (mode != MODE_NONE) {
		cli_evt("mode", "from=%s to=none", mode_name(mode));
		mode = MODE_NONE;
	}

	cli_ok(sh, NULL);

	return 0;
}

/* --- Protocol and client support --- */

int cmd_proto(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const names[] = {"text", "json", "human"};

	if (argc > 1) {
		size_t i;

		for (i = 0; i < ARRAY_SIZE(names) && strcmp(argv[1], names[i]) != 0; i++) {
		}

		if (i == ARRAY_SIZE(names)) {
			return cli_err(sh, -EINVAL, "proto: text json human");
		}

		cli_proto_set(sh, (enum cli_proto)i);
		cli_human_verbose_set(cli_flag(argc, argv, "verbose"));
	}

	cli_ok(sh, "proto=%s version=%d", cli_proto_str(cli_proto_get()), HA_TOOL_PROTO);

	return 0;
}

/* Limits of this build, so client libraries need not hard-code them */
int cmd_limits(const struct shell *sh, size_t argc, char **argv)
{

	for (size_t i = 0; i < ARRAY_SIZE(lc3_tab); i++) {
		const struct lc3_tab_cfg *c = &lc3_tab[i];

		cli_line(sh, "LC3 cfg=%s rate=%u frame_us=%u octets=%u tones=%u", c->name, c->sr_hz,
			 c->frame_us, c->octets, c->m_max - c->m_min + 1);
	}

	cli_ok(sh,
	       "identities=%d conns=%d bigs=%d bis_per_big=%d bis_total=%d subgroups=%d "
	       "level_min=%d "
	       "level_max=%d txpower_min=-40 txpower_max=8",
	       CONFIG_BT_ID_MAX - 1, CONFIG_BT_MAX_CONN, CONFIG_HA_BCAST_MAX_BIGS,
	       CONFIG_HA_BCAST_MAX_BIS, CONFIG_HA_BCAST_MAX_BIS_TOTAL,
	       CONFIG_HA_BCAST_MAX_SUBGROUPS, LC3_TAB_LEVEL_MIN, LC3_TAB_LEVEL_MAX);

	return 0;
}

/* Unique chip id, to tell several boards apart */
int cmd_id(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t id[16];
	char hex[2 * sizeof(id) + 1];
	ssize_t n;

	n = hwinfo_get_device_id(id, sizeof(id));
	if (n <= 0) {
		cli_err(sh, n < 0 ? (int)n : -ENOTSUP, "no device id");
		return -ENOTSUP;
	}

	for (ssize_t i = 0; i < n; i++) {
		snprintf(&hex[2 * i], 3, "%02X", id[i]);
	}

	cli_ok(sh, "id=%s", hex);

	return 0;
}

/* Uptime in us (events carry t= in ms): lets a client map board time to its own clock */
int cmd_time(const struct shell *sh, size_t argc, char **argv)
{

	cli_ok(sh, "us=%llu ms=%lld", k_ticks_to_us_floor64(k_uptime_ticks()), k_uptime_get());

	return 0;
}

static int cmd_evt_on_off(const struct shell *sh, size_t argc, char **argv, bool off)
{
	char list[200];

	if (!off && argc == 1) {
		cli_evt_filter_clear();
	}

	for (size_t i = 1; i < argc; i++) {
		if (cli_evt_filter(argv[i], off) != 0) {
			return cli_err(sh, -ENOMEM, "max 16 filtered events (%s)", argv[i]);
		}
	}

	cli_evt_filter_list(list, sizeof(list));
	cli_ok(sh, "off=%s", list[0] != '\0' ? list : "none");

	return 0;
}

int cmd_evt_off(const struct shell *sh, size_t argc, char **argv)
{
	return cmd_evt_on_off(sh, argc, argv, true);
}

int cmd_evt_on(const struct shell *sh, size_t argc, char **argv)
{
	return cmd_evt_on_off(sh, argc, argv, false);
}

int cmd_evt_list(const struct shell *sh, size_t argc, char **argv)
{
	return cmd_evt_on_off(sh, 0, argv, true);
}

/* Output health since boot: truncated lines, dropped JSON fields, lost events */
int cmd_cli_stats(const struct shell *sh, size_t argc, char **argv)
{
	struct cli_stats st;

	cli_stats_get(&st);
	cli_ok(sh, "lines_truncated=%u fields_dropped=%u evt_dropped=%u", st.lines_truncated,
	       st.fields_dropped, st.evt_dropped);

	return 0;
}

/*
 * Diagnostic: n events at once, faster than the event queue drains. Shows whether the host
 * keeps up; what the queue cannot hold is reported by 'dropped' events and 'cli stats'.
 */
int cmd_cli_burst(const struct shell *sh, size_t argc, char **argv)
{
	int err = 0;
	long n;

	n = cli_kv_long(argc, argv, "n", 64, &err);
	if (err != 0 || n < 1 || n > 1000) {
		return cli_err(sh, -EINVAL, "n=1..1000");
	}

	for (long i = 1; i <= n; i++) {
		cli_evt("burst", "i=%ld n=%ld", i, n);
	}

	cli_ok(sh, "n=%ld", n);

	return 0;
}

/* Commands with their accepted keys (CLI_KEYS), for the protocol consistency test */
int cmd_cli_spec(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	cli_spec_print(sh);
	cli_ok(sh, "commands=%u", (unsigned int)cli_cmds_count);

	return 0;
}

int cmd_help(const struct shell *sh, size_t argc, char **argv)
{
	return cli_help(sh, argc, argv);
}
