/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/* Air time of BIG events and the budget all broadcasts share. */

#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/audio/bap.h>

#include "cli.h"
#include "bcast/internal.h"

/*
 * Air time budget of all broadcasts together, from the controller's own reservations
 * (leabench/netcore.conf; Zephyr patch 0006 places every periodic role in a phase of its own):
 *   sum over the streaming BIGs of (airtime_us + BIG_OVERHEAD_US), scaled to 10 ms,
 *   + ADV_RESERVE_*_US  <=  10000 us
 * BIG_OVERHEAD_US: what the controller reserves around a BIG event (control subevent, radio
 * start-up EVENT_OVERHEAD_START_US = 733 us, end margin) plus the scheduling margin.
 * ADV_RESERVE_MULTI_US (two or more BIGs): one phase for the periodic advertising trains (all of
 * them share it, shifted by whole BIG intervals) as long as a receiver synchronized to a train
 * reserves it (1.9 ms on 2M PHY), one for the auxiliary advertising sets (~1 ms), and a gap of
 * ~0.7 ms left free so that the primary advertising gets out and receivers find the broadcasts.
 * Within it every BIG gets a phase free of the others and of the trains: no frame lost, also at
 * a receiver that stays synchronized to the train.
 * ADV_RESERVE_ONE_US (one BIG): its train and auxiliary set take their phase in one BIG event
 * out of 6 and 7 only, and the controller keeps their slots in every BIG event anyway
 * (big_ctlr_max_rtn), so for one BIG the controller's event limit is the one that counts:
 * measured lossless right up to it in every preset, PHY, RTN and encryption.
 * Verified in BabbleSim at the edge of the budget (README, tests/test_capacity.py); past it the
 * BIGs still went out but trains or primary advertising collided with them (17-50 % loss, or
 * broadcasts nobody could find).
 */

/* Air time of one BIG event as the controller lays it out: every BIS sends RTN+1 subevents
 * (BN=1, no pre-transmissions: Zephyr patch 0003) of the largest PDU of the BIG (all BISes use
 * the same Max_PDU), each plus the subevent spacing (T_MSS). Framed PDUs (44.1 kHz) carry a
 * 5 octet segmentation header.
 */
uint32_t big_bn(const struct bt_bap_qos_cfg *qos)
{
	if (qos->framing == BT_BAP_QOS_CFG_FRAMING_FRAMED || qos->interval == 0U) {
		return 1U;
	}

	return MIN(MAX((uint32_t)qos->latency * 1000U / qos->interval, 2U) - 1U, 7U);
}

uint32_t big_iso_us(const struct bt_bap_qos_cfg *qos)
{
	return qos->interval * big_bn(qos) / 1250U * 1250U;
}

uint32_t big_airtime_us(const struct big *big, const struct bt_bap_qos_cfg *qos)
{
	const uint32_t us_per_byte = qos->phy == BT_BAP_QOS_CFG_2M ? 4U : 8U;
	const uint32_t overhead = (qos->phy == BT_BAP_QOS_CFG_2M ? 2U : 1U) + 4U + 2U + 3U;
	uint32_t max_pdu = 0;

	for (int i = 0; i < big->bis_count; i++) {
		max_pdu = MAX(max_pdu, bis_octets(big, &big->bis[i]));
	}

	max_pdu += (qos->framing == BT_BAP_QOS_CFG_FRAMING_FRAMED ? 5U : 0U) +
		   (big->encrypted ? 4U : 0U);

	/* BN > 1: every BIS sends BN x (RTN+1) subevents per (longer) ISO interval */
	return big->bis_count * big_bn(qos) * (qos->rtn + 1U) *
	       ((max_pdu + overhead) * us_per_byte + 150U);
}

/*
 * The controller's own limit for one BIG (LE Create BIG, ull_adv_iso.c): the whole BIG event
 * must fit the ISO interval next to the slots it keeps for the BIG's auxiliary and periodic
 * advertising, or the BIG is refused (Invalid Parameters). For RTN > 0 it silently lowers RTN
 * until the event fits, so the tool checks it itself and refuses instead (a BIG with fewer
 * repetitions than asked would be a different test).
 *   airtime(rtn) + CTLR_RESERVE_US + 4 us x (AUX_ADV_IND data + AUX_SYNC_IND data) <= interval
 * CTLR_RESERVE_US: event overheads (3 x start-up + end margin), the control subevent and the PDU
 * headers. Fitted to the acceptance matrix of scripts/capacity.py accept (10 presets, 1M / 2M,
 * plain / encrypted, 1-8 BISes: every accepted BIG within the limit, every refused one past it;
 * the fit leaves 2.74-2.84 ms). Advertising goes on the 2M secondary PHY (4 us per octet).
 */
#define CTLR_RESERVE_US 2830U

/* Data octets of the BIG's AUX_ADV_IND (source.c update_adv_data) */
static uint32_t aux_data_len(const struct big *big)
{
	const size_t program = strlen(big->sg[0].program);
	uint32_t len = 7U + 2U + strlen(big->name); /* Broadcast ID, Broadcast Name */

	if (big->pbp_enabled) {
		len += 6U + (program > 0U ? 2U + program : 0U); /* PBP announcement */
	}

	return len;
}

/* Data octets of the AUX_SYNC_IND: BASE (estimate of what bt_cap builds) and the BIGInfo */
static uint32_t pa_data_len(const struct big *big)
{
	uint32_t len = 8U; /* AD header, UUID, presentation delay, subgroup count */

	for (int s = 0; s < big->sg_count; s++) {
		const struct subgroup *sg = &big->sg[s];
		const size_t program = strlen(sg->program);

		/* codec ID, codec configuration, streaming context, language, program, BIS count */
		len += 5U + 1U + 10U + 1U + 4U + (sg->lang[0] != '\0' ? 5U : 0U) +
		       (program > 0U ? 2U + program : 0U) + 1U;
	}

	return len + 8U * big->bis_count + (big->encrypted ? 24U : 0U);
}

int big_ctlr_max_rtn(const struct big *big, const struct bt_bap_qos_cfg *qos)
{
	const uint32_t reserve = CTLR_RESERVE_US + 4U * (aux_data_len(big) + pa_data_len(big));
	/* The ISO interval is a whole number of 1.25 ms: 44.1 kHz (framed, SDU every 10884 us)
	 * gets 10 ms events
	 */
	const uint32_t iso_interval = big_iso_us(qos);
	struct bt_bap_qos_cfg q = *qos;
	int best = -1;

	for (int rtn = 0; rtn <= qos->rtn; rtn++) {
		q.rtn = rtn;
		if (big_airtime_us(big, &q) + reserve > iso_interval) {
			break;
		}
		best = rtn;
	}

	return best;
}

/* Share of the budget period a BIG takes */
uint32_t big_cost_us(uint32_t air_us, uint32_t interval_us)
{
	return interval_us > 0U
		       ? (uint32_t)(((uint64_t)(air_us + BIG_OVERHEAD_US) * BUDGET_PERIOD_US +
				     interval_us - 1U) /
				    interval_us)
		       : BUDGET_PERIOD_US;
}

/* Budget used by the streaming BIGs other than `except` */
uint32_t budget_used_us(const struct big *except)
{
	uint32_t used = 0;

	for (int b = 0; b < MAX_BIGS; b++) {
		const struct big *other = &bcast_bigs[b];

		if (other != except && other->state == BIG_STREAMING) {
			used += big_cost_us(big_airtime_us(other, &other->qos),
					    big_iso_us(&other->qos));
		}
	}

	return used;
}

uint32_t budget_limit_us(const struct big *big)
{
	int bigs = 1;

	for (int b = 0; b < MAX_BIGS; b++) {
		if (&bcast_bigs[b] != big && bcast_bigs[b].state == BIG_STREAMING) {
			bigs++;
		}
	}

	return BUDGET_PERIOD_US - (bigs > 1 ? ADV_RESERVE_MULTI_US : ADV_RESERVE_ONE_US);
}

uint32_t budget_free_us(void)
{
	const uint32_t used = budget_used_us(NULL);
	const uint32_t limit = budget_limit_us(NULL);

	return used < limit ? limit - used : 0U;
}

int budget_fit_bis(const struct bt_bap_qos_cfg *qos, const struct bt_bap_lc3_preset *preset,
		   bool encrypted)
{
	static struct big probe; /* too large for the shell stack */
	const uint32_t free_us = budget_free_us();
	int bis_used = 0;
	int streaming = 0;
	int max_n;
	int fit = 0;

	/* Air time is not the only limit: a free BIG and free BISes of the controller too */
	for (int b = 0; b < MAX_BIGS; b++) {
		if (bcast_bigs[b].state == BIG_STREAMING) {
			streaming++;
			bis_used += bcast_bigs[b].bis_count;
		}
	}

	if (streaming >= MAX_BIGS) {
		return 0;
	}

	max_n = MIN(MAX_BIS, CONFIG_LEA_BCAST_MAX_BIS_TOTAL - bis_used);

	/* A BIG of n BISes of this kind, grown until it no longer fits */
	memset(&probe, 0, sizeof(probe));
	/* As a new BIG advertises by default (name, PBP): the controller's limit depends on it */
	snprintf(probe.name, sizeof(probe.name), "LEA Bench Auracast %d", MAX_BIGS);
	probe.pbp_enabled = true;
	probe.sg_count = 1;
	probe.sg[0].preset = *preset;
	probe.encrypted = encrypted;
	for (int n = 1; n <= max_n; n++) {
		probe.bis_count = n;
		if (big_cost_us(big_airtime_us(&probe, qos), big_iso_us(qos)) > free_us ||
		    big_ctlr_max_rtn(&probe, qos) < (int)qos->rtn) {
			break;
		}

		fit = n;
	}

	return fit;
}

/*
 * BIGs with different SDU intervals (7.5 / 10 ms / 44.1 kHz framed) repeat with a different
 * period, so their events meet every few events (bcast_validate refuses that unless forced). In
 * simulation a 7.5 ms BIG next to a 10 ms BIG lost 25-40 % of its frames. A broadcaster cannot
 * see the loss: the receiver must check it.
 */
void warn_interval_mix(const struct big *big)
{
	for (int b = 0; b < MAX_BIGS; b++) {
		const struct big *other = &bcast_bigs[b];

		if (other != big && other->state == BIG_STREAMING &&
		    big_iso_us(&other->qos) != big_iso_us(&big->qos)) {
			cli_evt("broadcast_warning",
				"big=%d reason=interval_mix other=%d interval_us=%u other_us=%u "
				"airtime_us=%u other_airtime_us=%u",
				big_number(big), big_number(other), big_iso_us(&big->qos),
				big_iso_us(&other->qos), big_airtime_us(big, &big->qos),
				big_airtime_us(other, &other->qos));
		}
	}
}
