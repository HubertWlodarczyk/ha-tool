/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Radio mode: unicast (bonding + le-source) or broadcast, never both (a unicast stream next to a
 * broadcast loses ISO events in the controller, see README). Switching clears the whole context
 * of the previous mode.
 */

#ifndef LEABENCH_MODE_H_
#define LEABENCH_MODE_H_

struct shell;

enum ha_mode {
	MODE_NONE,      /* after boot and 'reset': the first command of an area enters its mode */
	MODE_UNICAST,   /* identities, connections, bonds (bonding) and the LE Audio source */
	MODE_BROADCAST, /* Auracast broadcasts */
	MODE_COUNT,
};

/* Enters `needed` if no mode is active; otherwise prints ERR and returns -EBUSY unless it is the
 * current mode. Called by every command that changes radio state, after parsing its arguments;
 * a mode entered by a command that then fails is left again (cli_on_reply).
 */
int mode_require(const struct shell *sh, enum ha_mode needed);
const char *mode_str(void);

#endif /* LEABENCH_MODE_H_ */
