/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Auracast broadcast sources built by the tester (bcast/, commands in bcast/cmds.c).
 */

#ifndef HA_TOOL_BCAST_H_
#define HA_TOOL_BCAST_H_

int bcast_init(void);

/* Stops and deletes all broadcasts */
void bcast_reset(void);

/* Stops all broadcasts, keeping their configuration */
void bcast_stop_all(void);

/* Streaming BIGs and all configured ones (streaming included) */
void bcast_counts(int *active, int *configured);

/* "none", "draft" or "streaming" (of the most advanced broadcast) */
const char *bcast_state_str(void);

#endif /* HA_TOOL_BCAST_H_ */
