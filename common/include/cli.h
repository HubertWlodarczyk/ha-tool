/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Machine-parsable CLI helpers shared by ha_tool and dut_sim.
 *
 * Output protocol (one line each), 'text' (default) and 'human':
 *   OK [key=value ...]          final reply of a command that succeeded
 *   ERR <errno> <message> [hint="<next step>; ..."]  final reply of a command that failed
 *   EVT t=<ms> <name> [k=v ...] asynchronous event
 *   <TAG> [k=v ...]             data line printed before the final OK (e.g. DEV, BOND)
 * Values with spaces, quotes or '=' are printed "quoted" with \ escapes (cli_str).
 *
 * 'json' (JSON Lines, for client libraries): the same lines as one JSON object each,
 *   {"type":"ok",...}  {"type":"err","errno":-22,...,"msg":"..."}
 *   {"type":"evt","t":1234,"evt":"<event name>",...}  {"type":"data","tag":"DEV",...}
 * Values are strings; words that are not key=value go to "msg". The protocol is documented in
 * protocol/hatool.yaml and docs/protocol.md.
 */

#ifndef CLI_H_
#define CLI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>

#define CLI_ADDR_STR_LEN (BT_ADDR_STR_LEN + sizeof("/random"))

/* Longest k=v line (reply, data line or event body). A longer one is cut at a word boundary and
 * marked "truncated=1" (JSON: "truncated":true); 'cli stats' counts these.
 */
#define CLI_LINE_MAX 320

/* --- Output (cli_out.c) --- */

enum cli_proto {
	CLI_PROTO_TEXT,
	CLI_PROTO_JSON,
	CLI_PROTO_HUMAN, /* text + echo and prompt, for terminals */
};

void cli_proto_set(const struct shell *sh, enum cli_proto p);
enum cli_proto cli_proto_get(void);
const char *cli_proto_str(enum cli_proto p);

/* Final reply of a command that succeeded: "OK k=v ..." (fmt may be NULL) */
void cli_ok(const struct shell *sh, const char *fmt, ...) __printf_like(2, 3);

/* Final reply of a command that failed: "ERR <err> ..."; returns err, so a handler can
 * `return cli_err(sh, -EINVAL, "...");`
 */
int cli_err(const struct shell *sh, int err, const char *fmt, ...) __printf_like(3, 4);

/* cli_err() with the next step(s) to try, "; " separated: text/json add hint="...", human prints
 * them on lines of their own after "Try:"
 */
int cli_err_hint(const struct shell *sh, int err, const char *hint, const char *fmt, ...)
	__printf_like(4, 5);

/*
 * Collects the failures of a step instead of printing them, so one command can report several
 * outcomes (broadcast start --all): between cli_capture(buf, len) and cli_capture(NULL, 0)
 * cli_err() writes its message to buf (the first one) and prints nothing; cli_captured() is
 * the errno of that failure (0: none).
 */
void cli_capture(char *buf, size_t len);
int cli_captured(void);

/* Data line before the final reply: "<TAG> k=v ..." */
void cli_line(const struct shell *sh, const char *fmt, ...) __printf_like(2, 3);

/* A user supplied string for a k=v line: as is, or "quoted" if it has spaces, quotes or '='
 * ("none" if empty). buf holds the quoted form.
 */
const char *cli_str(const char *s, char *buf, size_t len);

/* cli_str() into a returned buffer, valid until the end of the full expression (C11 temporary):
 *   cli_ok(sh, "name=%s", CLI_Q(name));
 */
struct cli_text {
	char s[96];
};

struct cli_text cli_quote(const char *s);
#define CLI_Q(_s) (cli_quote(_s).s)

/* CLI_Q() of what the user typed, shortened to 40 characters ("..."): error messages echo it
 * without counting a truncated line
 */
struct cli_text cli_quote_short(const char *s);
#define CLI_QS(_s) (cli_quote_short(_s).s)

/* value / scale with one decimal ("1000.0", "-12.1"), e.g. mHz or 1/100 dB */
const char *cli_fmt_fixed(int32_t value, unsigned int scale, char *buf, size_t len);

/* cli_fmt_fixed() into a returned buffer, valid until the end of the full expression */
struct cli_num {
	char s[24];
};

struct cli_num cli_fixed(int32_t value, unsigned int scale);
#define CLI_FIXED(_v, _scale) (cli_fixed((_v), (_scale)).s)

/* Output health since boot ('cli stats') */
struct cli_stats {
	uint32_t lines_truncated; /* replies, data lines and events cut to CLI_LINE_MAX */
	uint32_t fields_dropped;  /* JSON fields that did not fit */
	uint32_t evt_dropped;     /* events lost because the queue was full */
};

void cli_stats_get(struct cli_stats *st);

/*
 * Called after every final reply (cli_ok: ok, cli_err: !ok) from the shell thread. Weak no-op;
 * an application overrides it, e.g. to undo what a failed command started.
 */
void cli_on_reply(bool ok);

/* --- Events (cli_evt.c) --- */

/* Asynchronous event "EVT t=<ms> <name> k=v ...", queued; safe from any context but ISRs */
void cli_evt(const char *name, const char *fmt, ...) __printf_like(2, 3);

/*
 * proto human: an event the application describes in words (out, a sentence) is printed so
 * instead of its EVT line; with cli_human_verbose() both. Weak: no event is described.
 */
bool cli_evt_human(const char *name, const char *body, char *out, size_t len);
void cli_human_verbose_set(bool on);
bool cli_human_verbose(void);

/* Value of key=value in an event body ("quoted" values unquoted); "" and false if absent */
bool cli_body_get(const char *body, const char *key, char *out, size_t len);

/* Events by name are not printed (and not queued) while filtered */
int cli_evt_filter(const char *name, bool off);
void cli_evt_filter_clear(void);
size_t cli_evt_filter_list(char *buf, size_t len);

/* --- Input (cli_parse.c) --- */

/* Value of a "key=value" argument, or NULL if absent */
const char *cli_kv(size_t argc, char **argv, const char *key);

/* Integer "key=value" argument (decimal or 0x hex); returns def if absent.
 * Sets *err on a malformed value.
 */
long cli_kv_long(size_t argc, char **argv, const char *key, long def, int *err);

/* Index of str in names[count] (NULL entries are skipped), or -EINVAL */
int cli_parse_enum(const char *str, const char *const *names, size_t count);

/* Parses "AA:BB:CC:DD:EE:FF[/random|/public]"; the type defaults to random */
int cli_parse_addr(const char *str, bt_addr_le_t *addr);
void cli_addr_str(const bt_addr_le_t *addr, char *buf, size_t len);

/* Parses a plain decimal argument */
int cli_parse_u8(const char *str, uint8_t *out);

/* Short name for a HCI disconnect reason or SMP error */
const char *cli_hci_reason_str(uint8_t reason);

/*
 * Keys (key=value) a command accepts; anything else is an error instead of being ignored
 * (a typo like frq= would silently fall back to the default). Defined next to the handler:
 *
 *   CLI_KEYS(bcast_tone, "bcast tone", "bis", "freq", "level");
 *   static int cmd_tone(...) { CLI_CHECK(bcast_tone, sh, argc, argv); ... }
 *
 * The sets are collected in an iterable section ('cli spec' lists them for the protocol test).
 */
struct cli_keyset {
	const char *cmd;
	const char *const *keys;
};

#define CLI_KEY_(_key) _key,

#define CLI_KEYS(_id, _cmd, ...)                                                                   \
	static const char *const cli_keys_##_id[] = {                                              \
		FOR_EACH_NONEMPTY_TERM(CLI_KEY_, (), __VA_ARGS__) NULL};                           \
	static const STRUCT_SECTION_ITERABLE(cli_keyset, cli_keyset_##_id) = {                     \
		.cmd = _cmd,                                                                       \
		.keys = cli_keys_##_id,                                                            \
	}

#define CLI_CHECK(_id, _sh, _argc, _argv)                                                          \
	do {                                                                                       \
		if (cli_check_keys(_sh, _argc, _argv, &cli_keyset_##_id) != 0) {                   \
			return -EINVAL;                                                            \
		}                                                                                  \
	} while (0)

int cli_check_keys(const struct shell *sh, size_t argc, char **argv, const struct cli_keyset *set);

#endif /* CLI_H_ */
