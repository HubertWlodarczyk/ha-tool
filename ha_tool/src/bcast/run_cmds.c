/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * 'broadcast' commands that run or read broadcasts: start / stop / restart (one BIG, or --all
 * with an outcome per BIG), show, list, budget, txpower.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/shell/shell.h>

#include "audio_tx.h"
#include "bcast.h"
#include "cli.h"
#include "cli_cmds.h"
#include "cli_tree.h"
#include "lc3_frames.h"
#include "mode.h"
#include "presets.h"
#include "bcast/internal.h"

static const char *pbp_str(const struct big *big)
{
	return !big->pbp_enabled ? "off" : (big->pbp_hq ? "hq" : "sq");
}

/* Starts one BIG (ERR printed): checks, then the controller; the outcome becomes its state */
static int start_big(const struct shell *sh, struct big *big, bool force)
{
	int err = bcast_validate(sh, big, force);

	if (err == 0) {
		err = big_start(big);
		if (err != 0) {
			cli_err(sh, err, "start of BIG %d failed", big_number(big));
		}
	}

	big->last_err = err;

	return err;
}

int cmd_broadcast_start(const struct shell *sh, size_t argc, char **argv)
{
	struct big *big;
	char hint[32];

	if (bcast_target(sh, argv[1], true, true, &big) != 0) {
		return -EINVAL;
	}

	if (big->state == BIG_STREAMING) {
		snprintf(hint, sizeof(hint), "broadcast %d restart", big_number(big));
		return cli_err_hint(sh, -EALREADY, hint, "BIG %d is already active",
				    big_number(big));
	}

	if (start_big(sh, big, cli_flag(argc, argv, "force")) != 0) {
		return -EIO;
	}

	cli_ok(sh, "big=%d state=active broadcast_id=0x%06X", big_number(big), big->broadcast_id);

	return 0;
}

int cmd_broadcast_stop(const struct shell *sh, size_t argc, char **argv)
{
	struct big *big;

	ARG_UNUSED(argc);

	if (bcast_target(sh, argv[1], true, true, &big) != 0) {
		return -EINVAL;
	}

	if (big->state != BIG_STREAMING) {
		return cli_err(sh, -EALREADY, "BIG %d is not active", big_number(big));
	}

	big_stop(big);
	cli_ok(sh, "big=%d state=%s", big_number(big), big_state_str(big));

	return 0;
}

int cmd_broadcast_restart(const struct shell *sh, size_t argc, char **argv)
{
	struct big *big;

	if (bcast_target(sh, argv[1], true, true, &big) != 0) {
		return -EINVAL;
	}

	big_stop(big);
	if (start_big(sh, big, cli_flag(argc, argv, "force")) != 0) {
		return -EIO;
	}

	cli_ok(sh, "big=%d state=active broadcast_id=0x%06X", big_number(big), big->broadcast_id);

	return 0;
}

enum all_op {
	ALL_START,
	ALL_STOP,
	ALL_RESTART,
};

/*
 * The operation on every configured BIG, one RESULT line each. Any failure makes the reply an
 * ERR: a partial success is never reported as a success.
 */
static int all_bigs(const struct shell *sh, size_t argc, char **argv, enum all_op op)
{
	static const char *const done[] = {"started", "stopped", "restarted"};
	const bool force = cli_flag(argc, argv, "force");
	char why[CLI_LINE_MAX / 2];
	char failed_list[MAX_BIGS * 4] = "";
	int first_err = 0;
	int failed_big = 0;
	int failed = 0;
	int count = 0;
	int done_n = 0; /* BIGs the operation was performed on (not already active / inactive) */

	if (!cli_flag(argc, argv, "all")) {
		return cli_err_hint(sh, -EINVAL, "broadcast <n> start; broadcast start --all",
				    "give a BIG number or --all");
	}

	if (mode_require(sh, MODE_BROADCAST) != 0) {
		return -EBUSY;
	}

	for (int b = 0; b < MAX_BIGS; b++) {
		struct big *big = &bcast_bigs[b];
		const char *result = done[op];
		int err = 0;

		if (big->state == BIG_NONE) {
			continue;
		}

		count++;
		why[0] = '\0';
		if (op == ALL_START && big->state == BIG_STREAMING) {
			result = "active";
		} else if (op == ALL_STOP) {
			if (big->state == BIG_STREAMING) {
				big_stop(big);
			} else {
				result = "inactive";
			}
		} else {
			if (op == ALL_RESTART) {
				big_stop(big);
			}

			cli_capture(why, sizeof(why));
			err = start_big(sh, big, force);
			cli_capture(NULL, 0);
		}

		if (err != 0) {
			const size_t pos = strlen(failed_list);

			result = "failed";
			if (first_err == 0) {
				first_err = err;
				failed_big = b + 1;
			}
			snprintf(&failed_list[pos], sizeof(failed_list) - pos, "%s%d",
				 failed++ > 0 ? ", " : "", b + 1);
		} else if (result == done[op]) {
			done_n++;
		}

		cli_line(sh, "RESULT big=%d name=%s result=%s errno=%d reason=%s", b + 1,
			 CLI_Q(big->name), result, err, CLI_Q(why[0] != '\0' ? why : "none"));
	}

	if (first_err != 0) {
		char hint[48];

		snprintf(hint, sizeof(hint), "broadcast %d show; broadcast budget", failed_big);
		return cli_err_hint(sh, first_err, hint, "%d of %d BIGs %s, %s %s failed", done_n,
				    count, done[op], failed > 1 ? "BIGs" : "BIG", failed_list);
	}

	cli_ok(sh, "count=%d %s=%d", count, done[op], done_n);

	return 0;
}

int cmd_broadcast_start_all(const struct shell *sh, size_t argc, char **argv)
{
	return all_bigs(sh, argc, argv, ALL_START);
}

int cmd_broadcast_stop_all(const struct shell *sh, size_t argc, char **argv)
{
	return all_bigs(sh, argc, argv, ALL_STOP);
}

int cmd_broadcast_restart_all(const struct shell *sh, size_t argc, char **argv)
{
	return all_bigs(sh, argc, argv, ALL_RESTART);
}

/* Human view of one BIG: a tree of its subgroups and BISes */
static void show_human(const struct shell *sh, const struct big *big,
		       const struct bt_bap_qos_cfg *qos, uint32_t air)
{
	shell_print(sh, "BIG %d %s  state=%s  id=0x%06X  %s  phy=%s interval=%s ms rtn=%u",
		    big_number(big), CLI_Q(big->name), big_state_str(big), big->broadcast_id,
		    big->encrypted ? "encrypted" : "plain",
		    qos->phy == BT_BAP_QOS_CFG_1M ? "1m" : "2m",
		    qos->interval == 7500 ? "7.5" : "10", qos->rtn);
	shell_print(sh, "  airtime %u us  pa %u ms  pbp %s", air, big->pa_ms, pbp_str(big));
	for (int s = 0; s < big->sg_count; s++) {
		const struct subgroup *sg = &big->sg[s];

		shell_print(sh, "%s Subgroup %d: %s, %s%s%s%s%s",
			    s + 1 < big->sg_count ? "|--" : "`--", s + 1, sg->preset_name,
			    audio_ctx_str(sg->ctx), sg->lang[0] != '\0' ? ", language " : "",
			    sg->lang, sg->program[0] != '\0' ? ", program " : "",
			    sg->program[0] != '\0' ? CLI_Q(sg->program) : "");
		for (int i = 0; i < big->bis_count; i++) {
			const struct bis *b = &big->bis[i];

			if (b->sg != s) {
				continue;
			}

			shell_print(sh, "%s   BIS %d: %s, %u Hz (actual %s), %d dBFS",
				    s + 1 < big->sg_count ? "|" : " ", i + 1, loc_str(b->loc),
				    b->tone.freq_hz, CLI_FIXED(b->freq_mhz, 1000),
				    b->tone.level_db);
		}
	}
}

int cmd_broadcast_show(const struct shell *sh, size_t argc, char **argv)
{
	struct bt_bap_qos_cfg qos;
	struct big *big;
	uint32_t air = 0;

	ARG_UNUSED(argc);

	if (bcast_target(sh, argv[1], false, true, &big) != 0) {
		return -EINVAL;
	}

	if (big->sg_count > 0 && big->bis_count > 0) {
		big_effective_qos(big, &qos);
		air = big_airtime_us(big, &qos);
	} else {
		memset(&qos, 0, sizeof(qos));
	}

	if (cli_proto_get() == CLI_PROTO_HUMAN) {
		show_human(sh, big, &qos, air);
	} else {
		subgroup_lines(sh, big);
		for (int i = 0; i < big->bis_count; i++) {
			bis_line(sh, big, i);
		}
	}

	cli_ok(sh,
	       "big=%d state=%s broadcast_id=0x%06X name=%s encrypted=%d pbp=%s subgroups=%u "
	       "bis=%u "
	       "phy=%s interval_us=%u rtn=%u sdu=%u pa_ms=%u airtime_us=%u fits=%d",
	       big_number(big), big_state_str(big), big->broadcast_id, CLI_Q(big->name),
	       big->encrypted, pbp_str(big), big->sg_count, big->bis_count,
	       qos.phy == BT_BAP_QOS_CFG_1M ? "1m" : "2m", qos.interval, qos.rtn, qos.sdu,
	       big->pa_ms, air,
	       qos.interval > 0 &&
		       budget_used_us(big) + big_cost_us(air, qos.interval) <=
			       budget_limit_us(big) &&
		       big_ctlr_max_rtn(big, &qos) >= (int)qos.rtn);

	return 0;
}

int cmd_broadcast_list(const struct shell *sh, size_t argc, char **argv)
{
	const bool human = cli_proto_get() == CLI_PROTO_HUMAN;
	uint8_t n = 0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (human) {
		int active;
		int configured;

		bcast_counts(&active, &configured);
		if (configured == 0) {
			shell_print(sh, "No BIGs yet. Create one: broadcast 1 create name=<name>");
		} else {
			shell_print(sh, "%-4s %-24s %-8s %-10s %-4s %s", "BIG", "NAME", "STATE",
				    "SUBGROUPS", "BIS", "ID");
		}
	}

	for (int b = 0; b < MAX_BIGS; b++) {
		const struct big *big = &bcast_bigs[b];

		if (big->state == BIG_NONE) {
			continue;
		}

		if (human) {
			shell_print(sh, "%-4d %-24s %-8s %-10u %-4u 0x%06X", b + 1,
				    CLI_Q(big->name), big_state_str(big), big->sg_count,
				    big->bis_count, big->broadcast_id);
		} else {
			cli_line(sh,
				 "BIG big=%d state=%s broadcast_id=0x%06X name=%s subgroups=%u "
				 "bis=%u",
				 b + 1, big_state_str(big), big->broadcast_id, CLI_Q(big->name),
				 big->sg_count, big->bis_count);
		}

		n++;
	}

	cli_ok(sh, "count=%u max=%d", n, MAX_BIGS);

	return 0;
}

/* Percent of the 10 ms budget period, one decimal */
static const char *pct(uint32_t us, char *buf, size_t len)
{
	return cli_fmt_fixed((int32_t)us, BUDGET_PERIOD_US / 100U, buf, len);
}

/* broadcast budget [preset=16_2] [rtn=N] [phy=1m|2m] [enc=0|1]: air time of the streaming BIGs
 * and how many BISes of that kind still fit into one more BIG
 */
int cmd_broadcast_budget(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const phys[] = {[BT_BAP_QOS_CFG_1M] = "1m", [BT_BAP_QOS_CFG_2M] = "2m"};
	const bool human = cli_proto_get() == CLI_PROTO_HUMAN;
	const char *phy = cli_kv(argc, argv, "phy");
	const struct bt_bap_lc3_preset *preset;
	struct bt_bap_qos_cfg qos;
	const char *name;
	char p1[16];
	char p2[16];
	int err = 0;
	long rtn, enc;

	if (mode_require(sh, MODE_BROADCAST) != 0) {
		return -EBUSY;
	}

	preset = preset_find(PRESET_BROADCAST, cli_kv(argc, argv, "preset") ?: "16_2", &name);
	rtn = cli_kv_long(argc, argv, "rtn", -1, &err);
	enc = cli_kv_long(argc, argv, "enc", 0, &err);
	if (preset == NULL || err != 0 || rtn > 15 ||
	    (phy != NULL && cli_parse_enum(phy, phys, ARRAY_SIZE(phys)) < 0)) {
		return cli_err(sh, -EINVAL, "preset=(%s) rtn=0..15 phy=1m|2m enc=0|1",
			       preset_names());
	}

	qos = preset->qos;
	if (rtn >= 0) {
		qos.rtn = rtn;
	}

	if (phy != NULL) {
		qos.phy = cli_parse_enum(phy, phys, ARRAY_SIZE(phys));
	}

	if (human) {
		char p3[16];

		/* The rest of the period is kept for advertising, PA and the controller */
		shell_print(sh, "Air time of each %u ms (broadcasts may use %s%%):",
			    BUDGET_PERIOD_US / 1000U, pct(budget_limit_us(NULL), p3, sizeof(p3)));
	}

	for (int b = 0; b < MAX_BIGS; b++) {
		const struct big *big = &bcast_bigs[b];
		uint32_t air;
		uint32_t cost;

		if (big->state != BIG_STREAMING) {
			continue;
		}

		air = big_airtime_us(big, &big->qos);
		cost = big_cost_us(air, big->qos.interval);
		if (human) {
			shell_print(sh, "  BIG %d %-22s %s%%", b + 1, CLI_Q(big->name),
				    pct(cost, p1, sizeof(p1)));
		} else {
			cli_line(
				sh,
				"BIG big=%d name=%s bis=%u interval_us=%u airtime_us=%u cost_us=%u "
				"cost_pct=%s",
				b + 1, CLI_Q(big->name), big->bis_count, big->qos.interval, air,
				cost, pct(cost, p1, sizeof(p1)));
		}
	}

	if (human) {
		shell_print(sh, "  Used:  %s%%", pct(budget_used_us(NULL), p1, sizeof(p1)));
		shell_print(sh, "  Free:  %s%%, enough for one more BIG of %d BIS (%s, rtn=%u)",
			    pct(budget_free_us(), p2, sizeof(p2)),
			    budget_fit_bis(&qos, preset, enc != 0), name, qos.rtn);
	}

	cli_ok(sh,
	       "used_us=%u free_us=%u used_pct=%s free_pct=%s budget_us=%u period_us=%u "
	       "big_overhead_us=%u preset=%s rtn=%u phy=%s enc=%ld next_big_bis=%d",
	       budget_used_us(NULL), budget_free_us(), pct(budget_used_us(NULL), p1, sizeof(p1)),
	       pct(budget_free_us(), p2, sizeof(p2)), budget_limit_us(NULL), BUDGET_PERIOD_US,
	       BIG_OVERHEAD_US, name, qos.rtn, phys[qos.phy], enc,
	       budget_fit_bis(&qos, preset, enc != 0));

	return 0;
}

/* broadcast <n> txpower <dBm>: TX power of the broadcast (advertising, PA, BIG), also live */
int cmd_broadcast_txpower(const struct shell *sh, size_t argc, char **argv)
{
	struct big *big;
	int8_t selected = 0;
	char *end;
	long dbm;
	int err;

	ARG_UNUSED(argc);

	if (bcast_target(sh, argv[1], true, true, &big) != 0) {
		return -EINVAL;
	}

	dbm = strtol(argv[2], &end, 10);
	if (*end != '\0' || dbm < -40 || dbm > 8) {
		return cli_err(sh, -EINVAL, "dBm: -40..8");
	}

	big->txpower = dbm;
	err = apply_txpower(big, &selected);
	if (err != 0) {
		return cli_err(sh, err, "TX power not set");
	}

	if (big->state == BIG_STREAMING) {
		cli_ok(sh, "big=%d txpower=%d", big_number(big), selected);
	} else {
		cli_ok(sh, "big=%d txpower=%ld applied=start", big_number(big), dbm);
	}

	return 0;
}
