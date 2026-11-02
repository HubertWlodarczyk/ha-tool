/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Command table and dispatcher: the commands of an application are a table generated from the
 * protocol specification (tools/protogen, e.g. ha_tool/src/cli_table.c). The shell only knows the
 * first word of each command; cli_dispatch() finds the command, checks the positional arguments,
 * keys (key=value) and flags (--name), and calls the handler. Unknown commands get the nearest
 * valid ones as a hint; 'help' is printed from the same table.
 *
 * A path word in braces is a target taken from the command line, e.g. "broadcast {big} start":
 * the handler gets it as its first positional argument (argv[1]), followed by the other words of
 * the line in their order.
 */

#ifndef CLI_TREE_H_
#define CLI_TREE_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/shell/shell.h>

typedef int (*cli_handler_t)(const struct shell *sh, size_t argc, char **argv);

#define CLI_ARGS_LIST UINT8_MAX /* args_max of a command taking a list of positional arguments */

struct cli_cmd {
	const char *path;         /* "dev create", "broadcast {big} bis add" */
	cli_handler_t handler;    /* gets argv[0] = last fixed word, then targets and the rest */
	uint8_t args_min;         /* positional arguments, targets included */
	uint8_t args_max;         /* or CLI_ARGS_LIST */
	const char *const *keys;  /* accepted key=value keys, NULL terminated */
	const char *const *flags; /* accepted --flags (without --), NULL terminated */
	const char *usage;        /* arguments, e.g. "<id> [sec=2|3|4]" */
	const char *help;         /* one line */
};

/* The application's table (generated) */
extern const struct cli_cmd cli_cmds[];
extern const size_t cli_cmds_count;

/* Shell handler of every first word of the table */
int cli_dispatch(const struct shell *sh, size_t argc, char **argv);

/* 'help [<words>...]': commands under a prefix, or one command in full */
int cli_help(const struct shell *sh, size_t argc, char **argv);

/* Whether "--name" is on the command line */
bool cli_flag(size_t argc, char **argv, const char *name);

/* The table's commands with their keys ('cli spec', protocol consistency test) */
void cli_spec_print(const struct shell *sh);

#endif /* CLI_TREE_H_ */
