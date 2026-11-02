/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/* Checks before 'broadcast <n> start': structure, codecs, controller limits and air time. */

#include <errno.h>
#include <stdio.h>
#include <stdbool.h>

#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/shell/shell.h>

#include "audio_tx.h"
#include "cli.h"
#include "bcast/internal.h"

/* A BIG needs subgroups, and every subgroup a BIS */
static int check_structure(const struct shell *sh, const struct big *big)
{
	char hint[64];

	if (big->sg_count == 0) {
		snprintf(hint, sizeof(hint), "broadcast %d subgroup add preset=16_2",
			 big_number(big));
		return cli_err_hint(sh, -EINVAL, hint,
				    "cannot start BIG %d: no subgroup configured", big_number(big));
	}

	if (big->bis_count == 0) {
		snprintf(hint, sizeof(hint), "broadcast %d bis add subgroup=1 location=mono",
			 big_number(big));
		return cli_err_hint(sh, -EINVAL, hint, "cannot start BIG %d: no BIS configured",
				    big_number(big));
	}

	for (int s = 0; s < big->sg_count; s++) {
		bool has_bis = false;

		for (int i = 0; i < big->bis_count; i++) {
			has_bis |= big->bis[i].sg == s;
		}

		if (!has_bis) {
			snprintf(hint, sizeof(hint), "broadcast %d bis add subgroup=%d",
				 big_number(big), s + 1);
			return cli_err_hint(sh, -EINVAL, hint,
					    "cannot start BIG %d: subgroup %d has "
					    "no BIS",
					    big_number(big), s + 1);
		}
	}

	return 0;
}

/* One SDU interval per BIG, codec configurations of the frame table, SDU size */
static int check_codecs(const struct shell *sh, const struct big *big,
			const struct bt_bap_qos_cfg *qos)
{
	/* 7.5 and 10 ms frames go into separate BIGs */
	for (int s = 1; s < big->sg_count; s++) {
		if (sg_frame_us(&big->sg[s]) != sg_frame_us(&big->sg[0])) {
			return cli_err(
				sh, -EINVAL,
				"subgroup %d frame %u us != subgroup 1 frame %u us: use another "
				"BIG",
				s + 1, sg_frame_us(&big->sg[s]), sg_frame_us(&big->sg[0]));
		}
	}

	for (int i = 0; i < big->bis_count; i++) {
		const struct bis *bis = &big->bis[i];
		const uint32_t sdu_us = bis_sdu_interval_us(big, bis);

		if (sdu_us != qos->interval) {
			return cli_err(sh, -EINVAL,
				       "BIS %d: frames of %u us need interval=%u (BIG has %u): use "
				       "another BIG",
				       i + 1, sdu_us, sdu_us, qos->interval);
		}

		if (bis_cfg(big, bis) == NULL) {
			return cli_err(sh, -ENOTSUP,
				       "BIS %d: no frames for rate=%u frame_us=%u octets=%u (%s)",
				       i + 1, bis_rate_hz(big, bis), sg_frame_us(&big->sg[bis->sg]),
				       bis_octets(big, bis), audio_cfg_names());
		}
	}

	if (qos->sdu > CONFIG_BT_ISO_TX_MTU) {
		return cli_err(sh, -EINVAL, "SDU %u > ISO TX MTU %d", qos->sdu,
			       CONFIG_BT_ISO_TX_MTU);
	}

	return 0;
}

/* Controller limits next to the broadcasts already streaming: BIS count, intervals, air time */
static int check_resources(const struct shell *sh, const struct big *big,
			   const struct bt_bap_qos_cfg *qos, bool force)
{
	uint32_t total = big->bis_count;
	uint32_t air;

	for (int b = 0; b < MAX_BIGS; b++) {
		const struct big *other = &bcast_bigs[b];

		if (other == big || other->state != BIG_STREAMING) {
			continue;
		}

		total += other->bis_count;

		/* BIGs of different intervals meet every few events; with the controller's
		 * reservation of air time + BIG_OVERHEAD_US per BIG they could only avoid each
		 * other if their events and the advertising trains fit into the greatest common
		 * divisor of the intervals (7.5 / 10 ms: 2.5 ms), i.e. practically never.
		 */
		if (!force && other->qos.interval != qos->interval) {
			return cli_err(
				sh, -EDOM,
				"interval_us=%u but BIG %d streams with %u: events of BIGs with "
				"different intervals collide (frames lost); --force to start "
				"anyway",
				qos->interval, b + 1, other->qos.interval);
		}
	}

	if (total > CONFIG_HA_BCAST_MAX_BIS_TOTAL) {
		return cli_err(sh, -ENOMEM, "max %d BISes over all broadcasts (controller limit)",
			       CONFIG_HA_BCAST_MAX_BIS_TOTAL);
	}

	air = big_airtime_us(big, qos);
	if (budget_used_us(big) + big_cost_us(air, qos->interval) > budget_limit_us(big)) {
		/* Short: it is also the reason of a RESULT line; the details: broadcast budget */
		return cli_err_hint(
			sh, -E2BIG, "broadcast budget",
			"not enough air time: BIG %d needs %u us, %u us free (lower rtn=, "
			"fewer BISes or a smaller preset)",
			big_number(big), big_cost_us(air, qos->interval),
			budget_limit_us(big) - MIN(budget_used_us(big), budget_limit_us(big)));
	}

	return 0;
}

int bcast_validate(const struct shell *sh, const struct big *big, bool force)
{
	struct bt_bap_qos_cfg qos;
	int err;

	err = check_structure(sh, big);
	if (err != 0) {
		return err;
	}

	big_effective_qos(big, &qos);
	err = check_codecs(sh, big, &qos);

	return err != 0 ? err : check_resources(sh, big, &qos, force);
}
