/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/* Command table dispatcher: matching, argument / key / flag checks, hints, help (cli_tree.h). */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include "cli.h"
#include "cli_tree.h"

#define PATH_WORDS_MAX 6
#define HINTS_MAX      4

/* An error line quotes at most this much of the command, and of the hint (fits CLI_LINE_MAX) */
#define ECHO_MAX 96
#define HINT_MAX 140

static bool is_key(const char *tok)
{
	return strchr(tok, '=') != NULL;
}

static bool is_flag(const char *tok)
{
	return tok[0] == '-' && tok[1] == '-' && tok[2] != '\0';
}

static bool is_target(const char *word, size_t len)
{
	return len > 2 && word[0] == '{' && word[len - 1] == '}';
}

/* Next word of a path: start and length; NULL at the end */
static const char *path_word(const char *p, size_t *len)
{
	while (*p == ' ') {
		p++;
	}

	if (*p == '\0') {
		return NULL;
	}

	*len = strcspn(p, " ");
	return p;
}

static bool word_eq(const char *word, size_t len, const char *tok)
{
	return strlen(tok) == len && strncmp(word, tok, len) == 0;
}

/*
 * Number of tokens a command's path takes from the line (fixed words and targets), 0 if it does
 * not match. Targets are recorded in target_idx.
 */
static size_t path_match(const struct cli_cmd *cmd, size_t argc, char **argv, size_t *target_idx,
			 size_t *targets)
{
	const char *p = cmd->path;
	size_t len;
	size_t i = 0;

	*targets = 0;
	while ((p = path_word(p, &len)) != NULL) {
		if (i >= argc) {
			return 0;
		}

		if (is_target(p, len)) {
			if (is_key(argv[i]) || is_flag(argv[i]) || *targets >= PATH_WORDS_MAX) {
				return 0;
			}

			target_idx[(*targets)++] = i;
		} else if (!word_eq(p, len, argv[i])) {
			return 0;
		}

		i++;
		p += len;
	}

	return i;
}

/* Last fixed word of a path (the handler's argv[0]) */
static const char *last_word(const char *path, size_t *out_len)
{
	const char *last = path;
	const char *p = path;
	size_t len;

	*out_len = strlen(path);
	while ((p = path_word(p, &len)) != NULL) {
		if (!is_target(p, len)) {
			last = p;
			*out_len = len;
		}

		p += len;
	}

	return last;
}

static int check_keys(const struct shell *sh, const struct cli_cmd *cmd, size_t argc, char **argv)
{
	for (size_t i = 1; i < argc; i++) {
		const char *eq = strchr(argv[i], '=');
		const size_t klen = eq != NULL ? (size_t)(eq - argv[i]) : 0;
		bool known = false;

		if (eq == NULL) {
			continue;
		}

		for (const char *const *k = cmd->keys; k != NULL && *k != NULL; k++) {
			if (strlen(*k) == klen && strncmp(*k, argv[i], klen) == 0) {
				known = true;
				break;
			}
		}

		if (!known) {
			char list[160];
			size_t pos = 0;

			list[0] = '\0';
			for (const char *const *k = cmd->keys;
			     k != NULL && *k != NULL && pos + 20 < sizeof(list); k++) {
				pos += snprintf(&list[pos], sizeof(list) - pos, "%s%s",
						pos > 0 ? "," : "", *k);
			}

			return cli_err(sh, -EINVAL, "unknown key key=%.*s cmd=%s allowed=%s",
				       (int)klen, argv[i], CLI_Q(cmd->path),
				       pos > 0 ? list : "none");
		}

		/* cli_kv() would silently take the first one */
		for (size_t j = 1; j < i; j++) {
			if (strncmp(argv[j], argv[i], klen + 1) == 0) {
				return cli_err(sh, -EINVAL, "duplicate key key=%.*s cmd=%s",
					       (int)klen, argv[i], CLI_Q(cmd->path));
			}
		}
	}

	return 0;
}

static int check_flags(const struct shell *sh, const struct cli_cmd *cmd, size_t argc, char **argv)
{
	for (size_t i = 1; i < argc; i++) {
		bool known = false;

		if (!is_flag(argv[i])) {
			continue;
		}

		for (const char *const *f = cmd->flags; f != NULL && *f != NULL; f++) {
			if (strcmp(*f, &argv[i][2]) == 0) {
				known = true;
				break;
			}
		}

		if (!known) {
			return cli_err(sh, -EINVAL, "unknown flag flag=%.40s cmd=%s", argv[i],
				       CLI_Q(cmd->path));
		}
	}

	return 0;
}

bool cli_flag(size_t argc, char **argv, const char *name)
{
	for (size_t i = 1; i < argc; i++) {
		if (is_flag(argv[i]) && strcmp(&argv[i][2], name) == 0) {
			return true;
		}
	}

	return false;
}

/* Edit distance of two short words (bounded: larger values only mean "far") */
static size_t edit_distance(const char *a, size_t alen, const char *b, size_t blen)
{
	uint8_t row[24];

	if (alen >= sizeof(row) || blen >= sizeof(row)) {
		return SIZE_MAX;
	}

	for (size_t j = 0; j <= blen; j++) {
		row[j] = (uint8_t)j;
	}

	for (size_t i = 1; i <= alen; i++) {
		uint8_t diag = row[0];

		row[0] = (uint8_t)i;
		for (size_t j = 1; j <= blen; j++) {
			const uint8_t up = row[j];
			const uint8_t cost = a[i - 1] == b[j - 1] ? 0U : 1U;

			row[j] = MIN(MIN(row[j] + 1U, row[j - 1] + 1U), diag + cost);
			diag = up;
		}
	}

	return row[blen];
}

/* Fixed words of the line matched by some command path: how deep the line is still valid */
static size_t valid_prefix(size_t argc, char **argv)
{
	size_t best = 0;

	for (size_t c = 0; c < cli_cmds_count; c++) {
		const char *p = cli_cmds[c].path;
		size_t len;
		size_t i = 0;

		while ((p = path_word(p, &len)) != NULL && i < argc &&
		       (is_target(p, len) ? !is_key(argv[i]) && !is_flag(argv[i])
					  : word_eq(p, len, argv[i]))) {
			i++;
			p += len;
		}

		best = MAX(best, i);
	}

	return best;
}

/*
 * Commands to suggest for a line that matches none: those continuing the valid prefix with a word
 * close to the wrong one (or every continuation if none is close), as "path usage; ...".
 */
static void suggest(size_t argc, char **argv, char *buf, size_t len)
{
	const size_t depth = valid_prefix(argc, argv);
	const char *wrong = depth < argc ? argv[depth] : NULL;
	size_t pos = 0;
	int found = 0;

	buf[0] = '\0';
	for (int pass = 0; pass < 2 && found == 0; pass++) {
		for (size_t c = 0; c < cli_cmds_count && found < HINTS_MAX; c++) {
			const struct cli_cmd *cmd = &cli_cmds[c];
			const char *p = cmd->path;
			size_t wlen = 0;
			size_t i = 0;

			/* the command must share the valid prefix */
			while ((p = path_word(p, &wlen)) != NULL && i < depth &&
			       (is_target(p, wlen) || word_eq(p, wlen, argv[i]))) {
				i++;
				p += wlen;
			}

			if (i < depth || p == NULL) {
				continue;
			}

			/* close: at most 2 edits away, or the same first two letters */
			if (pass == 0 &&
			    (wrong == NULL || (edit_distance(p, wlen, wrong, strlen(wrong)) > 2 &&
					       (wlen < 2 || strncmp(p, wrong, 2) != 0)))) {
				continue;
			}

			/* the usage only if short: the hint is meant to be read at a glance */
			const bool with_usage = cmd->usage[0] != '\0' && strlen(cmd->usage) <= 24;
			const int n = snprintf(&buf[pos], len - pos, "%s%s%s%s",
					       found > 0 ? "; " : "", cmd->path,
					       with_usage ? " " : "", with_usage ? cmd->usage : "");

			if (n < 0 || (size_t)n >= len - pos) {
				buf[pos] = '\0';
				return;
			}

			pos += n;
			found++;
		}
	}
}

/* A command path as the user writes it: {big} shown as <big> */
static void path_display(const char *path, char *buf, size_t len)
{
	size_t o = 0;

	for (; *path != '\0' && o + 1 < len; path++) {
		buf[o++] = *path == '{' ? '<' : (*path == '}' ? '>' : *path);
	}

	buf[o] = '\0';
}

static void line_str(size_t argc, char **argv, char *buf, size_t len)
{
	size_t pos = 0;

	buf[0] = '\0';
	for (size_t i = 0; i < argc && pos < len; i++) {
		const int n = snprintf(&buf[pos], len - pos, "%s%s", i > 0 ? " " : "", argv[i]);

		if (n < 0) {
			break;
		}

		pos += n;
	}
}

int cli_dispatch(const struct shell *sh, size_t argc, char **argv)
{
	const struct cli_cmd *best = NULL;
	size_t best_n = 0;
	size_t best_targets = 0;
	size_t best_idx[PATH_WORDS_MAX];
	char *args[CONFIG_SHELL_ARGC_MAX + 1];
	size_t nargs = 1;
	size_t positional = 0;
	size_t word_len;
	int err;

	for (size_t c = 0; c < cli_cmds_count; c++) {
		size_t idx[PATH_WORDS_MAX];
		size_t targets;
		const size_t n = path_match(&cli_cmds[c], argc, argv, idx, &targets);

		/* the longest path wins; on a tie the one with fewer targets (fixed words first) */
		if (n > best_n || (n == best_n && n > 0 && targets < best_targets)) {
			best = &cli_cmds[c];
			best_n = n;
			best_targets = targets;
			memcpy(best_idx, idx, targets * sizeof(idx[0]));
		}
	}

	if (best == NULL) {
		char line[ECHO_MAX];
		char hint[HINT_MAX];

		line_str(argc, argv, line, sizeof(line));
		suggest(argc, argv, hint, sizeof(hint));
		return cli_err_hint(sh, -EINVAL, hint[0] != '\0' ? hint : NULL,
				    "unknown command cmd=%s", CLI_QS(line));
	}

	/* handler argv: last fixed word, targets, then the rest of the line in its order */
	args[0] = (char *)last_word(best->path, &word_len);
	for (size_t t = 0; t < best_targets; t++) {
		args[nargs++] = argv[best_idx[t]];
	}

	for (size_t i = best_n; i < argc && nargs < ARRAY_SIZE(args) - 1; i++) {
		args[nargs++] = argv[i];
	}

	args[nargs] = NULL;

	for (size_t i = 1; i < nargs; i++) {
		positional += (!is_key(args[i]) && !is_flag(args[i])) ? 1U : 0U;
	}

	if (positional < best->args_min ||
	    (best->args_max != CLI_ARGS_LIST && positional > best->args_max)) {
		/* a word that is no argument may be a mistyped subcommand */
		if (positional > best->args_max && best->args_max == best_targets) {
			char line[ECHO_MAX];
			char hint[HINT_MAX];

			line_str(argc, argv, line, sizeof(line));
			suggest(argc, argv, hint, sizeof(hint));
			if (hint[0] == '\0') {
				/* nothing continues the line: the command it does match */
				path_display(best->path, hint, sizeof(hint));
			}

			return cli_err_hint(sh, -EINVAL, hint, "unknown command cmd=%s",
					    CLI_QS(line));
		}

		char usage[CLI_LINE_MAX / 2];

		return cli_err_hint(sh, -EINVAL, NULL, "wrong parameter count cmd=%s usage=%s",
				    CLI_Q(best->path), cli_str(best->usage, usage, sizeof(usage)));
	}

	err = check_keys(sh, best, nargs, args);
	err = err ?: check_flags(sh, best, nargs, args);
	if (err != 0) {
		return err;
	}

	/* the handler sees its own word as argv[0] (a NUL in the path is not possible: copy) */
	{
		char word[24];

		if (word_len >= sizeof(word)) {
			word_len = sizeof(word) - 1;
		}

		memcpy(word, args[0], word_len);
		word[word_len] = '\0';
		args[0] = word;

		return best->handler(sh, nargs, args);
	}
}

/* --- help --- */

static bool prefix_match(const struct cli_cmd *cmd, size_t argc, char **argv)
{
	const char *p = cmd->path;
	size_t len;

	for (size_t i = 0; i < argc; i++) {
		p = path_word(p, &len);
		if (p == NULL || !(is_target(p, len) || word_eq(p, len, argv[i]))) {
			return false;
		}

		p += len;
	}

	return true;
}

int cli_help(const struct shell *sh, size_t argc, char **argv)
{
	const bool human = cli_proto_get() == CLI_PROTO_HUMAN;
	int n = 0;

	/* argv[0] is "help" */
	for (size_t c = 0; c < cli_cmds_count; c++) {
		const struct cli_cmd *cmd = &cli_cmds[c];

		if (!prefix_match(cmd, argc - 1, &argv[1])) {
			continue;
		}

		char path[48];

		path_display(cmd->path, path, sizeof(path));
		if (human) {
			shell_print(sh, "  %s%s%s", path, cmd->usage[0] != '\0' ? " " : "",
				    cmd->usage);
			shell_print(sh, "      %s", cmd->help);
		} else {
			char usage[CLI_LINE_MAX / 2];
			char doc[CLI_LINE_MAX / 2];

			cli_line(sh, "HELP cmd=%s usage=%s doc=%s", CLI_Q(path),
				 cli_str(cmd->usage[0] != '\0' ? cmd->usage : "-", usage,
					 sizeof(usage)),
				 cli_str(cmd->help, doc, sizeof(doc)));
		}

		n++;
	}

	if (n == 0) {
		char line[ECHO_MAX];
		char hint[HINT_MAX];

		line_str(argc - 1, &argv[1], line, sizeof(line));
		suggest(argc - 1, &argv[1], hint, sizeof(hint));
		return cli_err_hint(sh, -EINVAL, hint[0] != '\0' ? hint : NULL,
				    "unknown command cmd=%s", CLI_QS(line));
	}

	cli_ok(sh, "commands=%d", n);

	return 0;
}

void cli_spec_print(const struct shell *sh)
{
	for (size_t c = 0; c < cli_cmds_count; c++) {
		const struct cli_cmd *cmd = &cli_cmds[c];
		char keys[160];
		size_t pos = 0;

		keys[0] = '\0';
		for (const char *const *k = cmd->keys;
		     k != NULL && *k != NULL && pos + 20 < sizeof(keys); k++) {
			pos += snprintf(&keys[pos], sizeof(keys) - pos, "%s%s", pos > 0 ? "," : "",
					*k);
		}

		cli_line(sh, "SPEC cmd=%s keys=%s", CLI_Q(cmd->path), pos > 0 ? keys : "none");
	}
}
