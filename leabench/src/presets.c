/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include <stddef.h>
#include <string.h>

#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/sys/util.h>

#include "lc3_frames.h"
#include "presets.h"

struct preset_entry {
	const char *name;
	struct bt_bap_lc3_preset unicast;
	struct bt_bap_lc3_preset broadcast;
};

#define LOC BT_AUDIO_LOCATION_MONO_AUDIO
#define CTX BT_AUDIO_CONTEXT_TYPE_UNSPECIFIED

#define ENTRY(_name, _cfg)                                                                         \
	{.name = (_name),                                                                          \
	 .unicast = BT_BAP_LC3_UNICAST_PRESET_##_cfg(LOC, CTX),                                    \
	 .broadcast = BT_BAP_LC3_BROADCAST_PRESET_##_cfg(LOC, CTX)}

#define BOTH(_n) ENTRY(#_n, _n##_1), ENTRY(#_n "_hr", _n##_2)

/* Every LC3 configuration of BAP: 7.5 ms (_1, 3, 5) and 10 ms (_2, 4, 6) frames, 8..48 kHz.
 * All of them are in the frame table (lc3_frames.c).
 */
static const struct preset_entry presets[] = {
	BOTH(8_1),  BOTH(8_2),  BOTH(16_1),  BOTH(16_2),  BOTH(24_1), BOTH(24_2),
	BOTH(32_1), BOTH(32_2), BOTH(441_1), BOTH(441_2), BOTH(48_1), BOTH(48_2),
	BOTH(48_3), BOTH(48_4), BOTH(48_5),  BOTH(48_6),
};

/* Every LC3 configuration of the frame table, plain and high reliability */
BUILD_ASSERT(ARRAY_SIZE(presets) == 2 * LC3_TAB_COUNT, "one preset pair per table entry");

const struct bt_bap_lc3_preset *preset_find(enum preset_kind kind, const char *name,
					    const char **canonical)
{
	for (size_t i = 0; i < ARRAY_SIZE(presets); i++) {
		if (strcmp(presets[i].name, name) == 0) {
			if (canonical != NULL) {
				*canonical = presets[i].name;
			}

			return kind == PRESET_UNICAST ? &presets[i].unicast : &presets[i].broadcast;
		}
	}

	return NULL;
}

const char *preset_names(void)
{
	return "8_1 8_2 16_1 16_2 24_1 24_2 32_1 32_2 441_1 441_2 48_1..48_6 (+_hr)";
}
