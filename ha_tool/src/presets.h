/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef HA_TOOL_PRESETS_H_
#define HA_TOOL_PRESETS_H_

#include <zephyr/bluetooth/audio/bap_lc3_preset.h>

enum preset_kind {
	PRESET_UNICAST,
	PRESET_BROADCAST,
};

/* LC3 preset by name: 8_1 .. 48_6 (all of BAP), "_hr" suffix for the high reliability variant.
 * Returns the canonical (static) name through *canonical if not NULL.
 */
const struct bt_bap_lc3_preset *preset_find(enum preset_kind kind, const char *name,
					    const char **canonical);

/* Space separated list of names, for error messages */
const char *preset_names(void);

#endif /* HA_TOOL_PRESETS_H_ */
