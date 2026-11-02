/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef CLI_INTERNAL_H_
#define CLI_INTERNAL_H_

#include <stdarg.h>
#include <stdbool.h>

#include <zephyr/shell/shell.h>

/* One output line as JSON: {"type":<type><head>,"k":"v",...,"msg":"<other words>"}.
 * tag_first: the first word is a data line tag (DEV, BIS, ...).
 */
void cli_print_json(const struct shell *sh, const char *type, const char *head, const char *body,
		    bool tag_first);

/* k=v line into buf[CLI_LINE_MAX], cut at a word boundary and marked if too long */
int cli_vformat_line(char *buf, const char *fmt, va_list ap);

/* Counts a truncated line ('cli stats') */
void cli_note_truncated(void);

/* Event part of 'cli stats' (cli_evt.c) */
struct cli_stats;
void cli_evt_stats_get(struct cli_stats *st);

#endif /* CLI_INTERNAL_H_ */
