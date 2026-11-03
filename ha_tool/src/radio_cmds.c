/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * 'radio schedule': the controller's real schedule (vendor Read Schedule, Zephyr patch 0013)
 * instead of the budget's model. Every running role with its next event, period and reserved
 * slot, the reservations of scheduled roles (BIG, PA, AUX, ACL, CIG) that overlap within a 60 ms
 * window (the hyperperiod of the usual 7.5 / 10 / 60 ms periods), and in proto human a timeline
 * of that window.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/hci_vs.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include "cli.h"
#include "cli_cmds.h"
#include "hci_util.h"

#define WINDOW_US  60000U
#define COLUMN_US  500U /* timeline resolution */
#define COLUMNS    (WINDOW_US / COLUMN_US)
#define OCC_MAX    8 /* occurrences of one role kept per window, for the overlap count */
#define EVENTS_MAX (WINDOW_US / 7500U + 1U)

static const char *role_name(uint8_t role)
{
	static const char *const names[] = {"other", "adv",  "aux", "pa",
					    "big",   "scan", "acl", "cig"};

	return role < ARRAY_SIZE(names) ? names[role] : "other";
}

static char role_letter(uint8_t role)
{
	static const char letters[] = "?AXPBSLC";

	return role < sizeof(letters) - 1U ? letters[role] : '?';
}

/* Start of occurrence k of entry e within the window, or UINT32_MAX past it */
static uint32_t occurrence(const struct bt_hci_vs_sched_entry *e, uint32_t k)
{
	const uint64_t start = (uint64_t)e->next_us + (uint64_t)k * e->period_us;

	if ((k > 0U && e->period_us == 0U) || start >= WINDOW_US) {
		return UINT32_MAX;
	}

	return (uint32_t)start;
}

/* Roles whose events move or yield: primary advertising (random delay of up to 10 ms) and
 * scanning (unreserved) never collide with a reserved role, they wait
 */
static bool yields(const struct bt_hci_vs_sched_entry *e)
{
	return e->role == BT_HCI_VS_SCHED_ROLE_ADV || e->role == BT_HCI_VS_SCHED_ROLE_SCAN;
}

/* Reservations of two different scheduled roles that overlap in the window */
static int count_overlaps(const struct bt_hci_vs_sched_entry *e, int n)
{
	int overlaps = 0;

	for (int a = 0; a < n; a++) {
		for (int b = a + 1; b < n; b++) {
			if (yields(&e[a]) || yields(&e[b])) {
				continue;
			}

			for (uint32_t i = 0; i < EVENTS_MAX; i++) {
				const uint32_t sa = occurrence(&e[a], i);

				if (sa == UINT32_MAX) {
					break;
				}

				for (uint32_t j = 0; j < EVENTS_MAX; j++) {
					const uint32_t sb = occurrence(&e[b], j);

					if (sb == UINT32_MAX) {
						break;
					}

					overlaps +=
						sa < sb + e[b].slot_us && sb < sa + e[a].slot_us;
				}
			}
		}
	}

	return overlaps;
}

static void timeline(const struct shell *sh, const struct bt_hci_vs_sched_entry *e, int n)
{
	char line[COLUMNS + 1];
	uint8_t used[COLUMNS] = {0};

	shell_print(sh,
		    "Timeline of %u ms, one column = %u us (A adv, X aux, P pa, B big, S scan, "
		    "L acl, C cig; * two roles at once):",
		    WINDOW_US / 1000U, COLUMN_US);
	for (int r = 0; r < n; r++) {
		memset(line, '.', COLUMNS);
		line[COLUMNS] = '\0';
		for (uint32_t k = 0; k < EVENTS_MAX * 8U; k++) {
			const uint32_t start = occurrence(&e[r], k);

			if (start == UINT32_MAX) {
				break;
			}

			for (uint32_t c = start / COLUMN_US;
			     c < COLUMNS && c * COLUMN_US < start + MAX(e[r].slot_us, 1U); c++) {
				line[c] = role_letter(e[r].role);
				used[c]++;
			}
		}

		shell_print(sh, "%-5s %3u  %s", role_name(e[r].role), e[r].ticker_id, line);
	}

	for (uint32_t c = 0; c < COLUMNS; c++) {
		line[c] = used[c] > 1U ? '*' : ' ';
	}
	shell_print(sh, "           %s", line);
}

int cmd_radio_schedule(const struct shell *sh, size_t argc, char **argv)
{
	struct bt_hci_vs_sched_entry e[BT_HCI_VS_SCHED_ENTRIES_MAX];
	int n;
	int overlaps;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	n = hci_read_schedule(e, ARRAY_SIZE(e));
	if (n < 0) {
		return cli_err(sh, n, "the controller has no Read Schedule command (patch 0013)");
	}

	overlaps = count_overlaps(e, n);
	if (cli_proto_get() == CLI_PROTO_HUMAN) {
		timeline(sh, e, n);
	} else {
		for (int i = 0; i < n; i++) {
			cli_line(sh, "SLOT role=%s id=%u next_us=%u period_us=%u slot_us=%u",
				 role_name(e[i].role), e[i].ticker_id, e[i].next_us, e[i].period_us,
				 e[i].slot_us);
		}
	}

	cli_ok(sh, "count=%d overlaps=%d window_us=%u", n, overlaps, WINDOW_US);

	return 0;
}
