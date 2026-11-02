/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/* Air time of BIG events and the budget all broadcasts share. */

#include <string.h>

#include <zephyr/bluetooth/audio/bap.h>

#include "cli.h"
#include "bcast/internal.h"

/*
 * Air time budget of all broadcasts together, from the controller's own reservations
 * (ha_tool/netcore.conf; Zephyr patch 0006 places every periodic role in a phase of its own):
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
 * out of 6 and 7 only, so the advertising finds room in the others (measured lossless up to
 * 8.5 ms of air); the earlier, conservative limit of 6.5 ms is kept.
 * Verified in BabbleSim at the edge of the budget (README, tests/test_capacity.py); past it the
 * BIGs still went out but trains or primary advertising collided with them (17-50 % loss, or
 * broadcasts nobody could find).
 */

/* Air time of one BIG event as the controller lays it out: every BIS sends RTN+1 subevents
 * (BN=1, no pre-transmissions: Zephyr patch 0003) of the largest PDU of the BIG (all BISes use
 * the same Max_PDU), each plus the subevent spacing (T_MSS). Framed PDUs (44.1 kHz) carry a
 * 5 octet segmentation header.
 */
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

	return big->bis_count * (qos->rtn + 1U) * ((max_pdu + overhead) * us_per_byte + 150U);
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
					    other->qos.interval);
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

	max_n = MIN(MAX_BIS, CONFIG_HA_BCAST_MAX_BIS_TOTAL - bis_used);

	/* A BIG of n BISes of this kind, grown until it no longer fits */
	memset(&probe, 0, sizeof(probe));
	probe.sg_count = 1;
	probe.sg[0].preset = *preset;
	probe.encrypted = encrypted;
	for (int n = 1; n <= max_n; n++) {
		probe.bis_count = n;
		if (big_cost_us(big_airtime_us(&probe, qos), qos->interval) > free_us) {
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
		    other->qos.interval != big->qos.interval) {
			cli_evt("broadcast_warning",
				"big=%d reason=interval_mix other=%d interval_us=%u other_us=%u "
				"airtime_us=%u other_airtime_us=%u",
				big_number(big), big_number(other), big->qos.interval,
				other->qos.interval, big_airtime_us(big, &big->qos),
				big_airtime_us(other, &other->qos));
		}
	}
}
