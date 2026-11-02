/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * CLI output: OK / ERR / data lines in the selected protocol (text k=v, JSON Lines, or human:
 * text with echo and prompt), quoting of user strings.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include "cli.h"
#include "cli_internal.h"

/* Line limits: k=v text and its JSON form (escaping can make it longer) */
#define JSON_TAIL_MAX sizeof(",\"truncated\":true,\"dropped_fields\":999}")
#define JSON_LINE_MAX (2 * CLI_LINE_MAX + JSON_TAIL_MAX)
#define TRUNC_MARK    " truncated=1"

/* Room for a useful part of the line before the truncation mark */
BUILD_ASSERT(CLI_LINE_MAX >= 4 * sizeof(TRUNC_MARK), "CLI_LINE_MAX too small");

static atomic_t lines_truncated;
static atomic_t fields_dropped;

void cli_stats_get(struct cli_stats *st)
{
	st->lines_truncated = atomic_get(&lines_truncated);
	st->fields_dropped = atomic_get(&fields_dropped);
	cli_evt_stats_get(st);
}

void cli_note_truncated(void)
{
	atomic_inc(&lines_truncated);
}

/* --- Output protocol: text (k=v), json (JSON Lines) or human (text + echo and prompt) --- */

static enum cli_proto proto = CLI_PROTO_TEXT;
static bool human_verbose;

void cli_human_verbose_set(bool on)
{
	human_verbose = on;
}

bool cli_human_verbose(void)
{
	return human_verbose;
}

enum cli_proto cli_proto_get(void)
{
	return proto;
}

const char *cli_proto_str(enum cli_proto p)
{
	return p == CLI_PROTO_JSON ? "json" : p == CLI_PROTO_HUMAN ? "human" : "text";
}

void cli_proto_set(const struct shell *sh, enum cli_proto p)
{
	proto = p;
	shell_echo_set(sh, p == CLI_PROTO_HUMAN);
	(void)shell_prompt_change(sh, p == CLI_PROTO_HUMAN ? "ha> " : "");
}

/* --- JSON writer: always produces a closed object; what does not fit is counted --- */

struct json_out {
	char buf[JSON_LINE_MAX];
	size_t pos;
	unsigned int dropped; /* fields left out for lack of space */
	bool truncated;       /* the k=v source line was already cut */
};

/* Length of s[0..n) once escaped */
static size_t json_escaped_len(const char *s, size_t n)
{
	size_t len = 0;

	for (size_t i = 0; i < n; i++) {
		const unsigned char c = s[i];

		len += (c == '"' || c == '\\') ? 2 : (c < 0x20 ? 6 : 1);
	}

	return len;
}

static void json_put_escaped(struct json_out *j, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		const unsigned char c = s[i];

		if (c == '"' || c == '\\') {
			j->buf[j->pos++] = '\\';
			j->buf[j->pos++] = c;
		} else if (c < 0x20) {
			j->pos += snprintf(&j->buf[j->pos], 7, "\\u%04x", c);
		} else {
			j->buf[j->pos++] = c;
		}
	}
}

/* ,"key":"value" if it fits (keeping room for the closing tail), else counted as dropped */
static void json_field(struct json_out *j, const char *key, size_t key_len, const char *val,
		       size_t val_len)
{
	const size_t need = json_escaped_len(key, key_len) + json_escaped_len(val, val_len) + 6;

	if (j->pos >= sizeof(j->buf) || need + JSON_TAIL_MAX >= sizeof(j->buf) - j->pos) {
		j->dropped++;
		return;
	}

	j->buf[j->pos++] = ',';
	j->buf[j->pos++] = '"';
	json_put_escaped(j, key, key_len);
	j->buf[j->pos++] = '"';
	j->buf[j->pos++] = ':';
	j->buf[j->pos++] = '"';
	json_put_escaped(j, val, val_len);
	j->buf[j->pos++] = '"';
}

/* Appends formatted text; never moves pos past the end (keeps room for the closing brace) */
static void json_printf(struct json_out *j, const char *fmt, ...)
{
	const size_t room = sizeof(j->buf) - 1U - MIN(j->pos, sizeof(j->buf) - 1U);
	va_list ap;
	int n;

	if (room < 2U) {
		return;
	}

	va_start(ap, fmt);
	n = vsnprintf(&j->buf[j->pos], room, fmt, ap);
	va_end(ap);

	if (n > 0) {
		j->pos += MIN((size_t)n, room - 1U);
	}
}

static void json_close(struct json_out *j)
{
	if (j->truncated) {
		json_printf(j, ",\"truncated\":true");
	}

	if (j->dropped > 0) {
		json_printf(j, ",\"dropped_fields\":%u", MIN(j->dropped, 999U));
		atomic_add(&fields_dropped, (atomic_val_t)j->dropped);
	}

	j->pos = MIN(j->pos, sizeof(j->buf) - 2U); /* json_printf/json_field keep this room */
	j->buf[j->pos++] = '}';
	j->buf[j->pos] = '\0';
}

/* Next token of a k=v line; a value may be "quoted" (see cli_str). Returns its length. */
static size_t next_token(const char **p, const char **eq)
{
	const char *s = *p;
	const char *t = s;
	bool quoted = false;

	*eq = NULL;
	while (*t != '\0' && (quoted || *t != ' ')) {
		if (*t == '=' && *eq == NULL && !quoted) {
			*eq = t;
		} else if (*t == '"' && *eq != NULL) {
			quoted = !quoted;
		} else if (*t == '\\' && quoted && t[1] != '\0') {
			t++;
		}

		t++;
	}

	*p = t;
	while (**p == ' ') {
		(*p)++;
	}

	return t - s;
}

/* Copies a value, removing the quotes and escapes of cli_str() */
static size_t unquote(const char *v, size_t n, char *out, size_t len)
{
	size_t o = 0;

	if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
		for (size_t i = 1; i + 1 < n && o + 1 < len; i++) {
			if (v[i] == '\\' && i + 2 < n) {
				i++;
			}

			out[o++] = v[i];
		}
	} else {
		o = MIN(n, len - 1);
		memcpy(out, v, o);
	}

	out[o] = '\0';
	return o;
}

/*
 * One output line as JSON: {"type":<type><head>,"k":"v",...,"msg":"<other words>"}.
 * tag_first: the first word is a data line tag (DEV, BIS, ...). A source line ending with the
 * truncation mark (see format_line) becomes "truncated":true.
 */
void cli_print_json(const struct shell *sh, const char *type, const char *head, const char *body,
		    bool tag_first)
{
	static struct json_out j; /* too large for the callers' stacks; callers are serialized */
	static K_MUTEX_DEFINE(lock);
	char msg[CLI_LINE_MAX];
	char val[CLI_LINE_MAX];
	size_t mlen = 0;
	size_t blen = body != NULL ? strlen(body) : 0;
	const char *p = body != NULL ? body : "";
	bool first = true;

	k_mutex_lock(&lock, K_FOREVER);
	j.dropped = 0;
	j.truncated = blen >= strlen(TRUNC_MARK) &&
		      strcmp(&p[blen - strlen(TRUNC_MARK)], TRUNC_MARK) == 0;
	if (j.truncated) {
		blen -= strlen(TRUNC_MARK);
	}

	j.pos = 0;
	json_printf(&j, "{\"type\":\"%s\"%s", type, head);
	while (p < body + blen) {
		const char *tok = p;
		const char *eq;
		size_t n = next_token(&p, &eq);

		n = MIN(n, (size_t)(body + blen - tok));
		if (first && tag_first && eq == NULL) {
			json_field(&j, "tag", 3, tok, n);
		} else if (eq != NULL) {
			const size_t vn = unquote(eq + 1, tok + n - eq - 1, val, sizeof(val));

			json_field(&j, tok, eq - tok, val, vn);
		} else if (mlen + n + 2 < sizeof(msg)) {
			if (mlen > 0) {
				msg[mlen++] = ' ';
			}

			memcpy(&msg[mlen], tok, n);
			mlen += n;
		} else {
			j.dropped++;
		}

		first = false;
	}

	if (mlen > 0) {
		json_field(&j, "msg", 3, msg, mlen);
	}

	json_close(&j);
	shell_print(sh, "%s", j.buf);
	k_mutex_unlock(&lock);
}

/*
 * Formats a k=v line into buf (CLI_LINE_MAX). If it does not fit, the end is replaced by
 * TRUNC_MARK (which cli_print_json turns into "truncated":true) and the count goes up.
 * Returns the length.
 */
int cli_vformat_line(char *buf, const char *fmt, va_list ap)
{
	int len;

	if (fmt == NULL || fmt[0] == '\0') {
		buf[0] = '\0';
		return 0;
	}

	len = vsnprintf(buf, CLI_LINE_MAX, fmt, ap);
	if (len < 0) {
		buf[0] = '\0';
		return 0;
	}

	if (len >= CLI_LINE_MAX) {
		const size_t cut = CLI_LINE_MAX - sizeof(TRUNC_MARK);

		/* cut at a word boundary, so no half value is left */
		size_t end = cut;

		while (end > 0 && buf[end] != ' ') {
			end--;
		}

		memcpy(&buf[end > 0 ? end : cut], TRUNC_MARK, sizeof(TRUNC_MARK));
		cli_note_truncated();
		len = strlen(buf);
	}

	return len;
}

__weak void cli_on_reply(bool ok)
{
	ARG_UNUSED(ok);
}

/* Human form of a hint: "Try:" and one indented line per alternative ("; " separated) */
static void print_hint_human(const struct shell *sh, const char *hint)
{
	const char *p = hint;

	shell_print(sh, "Try:");
	while (*p != '\0') {
		const char *end = strstr(p, "; ");
		const size_t n = end != NULL ? (size_t)(end - p) : strlen(p);

		shell_print(sh, "  %.*s", (int)n, p);
		p += n + (end != NULL ? 2U : 0U);
	}
}

/* While set, failures are written here instead of printed (cli_capture) */
static char *capture_buf;
static size_t capture_len;
static int capture_err;

void cli_capture(char *buf, size_t len)
{
	capture_buf = len > 0 ? buf : NULL;
	capture_len = len;
	capture_err = 0;
	if (capture_buf != NULL) {
		capture_buf[0] = '\0';
	}
}

int cli_captured(void)
{
	return capture_err;
}

static void vprint(const struct shell *sh, const char *type, int err, const char *hint,
		   const char *fmt, va_list ap)
{
	char buf[CLI_LINE_MAX];
	int len = cli_vformat_line(buf, fmt, ap);

	if (capture_buf != NULL && type[0] == 'e') {
		if (capture_err == 0) { /* the first failure explains the rest */
			snprintf(capture_buf, capture_len, "%s", buf);
			capture_err = err;
		}

		return;
	}

	if (hint != NULL && proto != CLI_PROTO_HUMAN) {
		char q[CLI_LINE_MAX / 2];
		const int n = snprintf(&buf[len], sizeof(buf) - len, "%shint=%s",
				       len > 0 ? " " : "", cli_str(hint, q, sizeof(q)));

		if (n < 0 || (size_t)n >= sizeof(buf) - len) {
			buf[len] = '\0'; /* no room: the message without the hint */
			cli_note_truncated();
		} else {
			len += n;
		}
	}

	if (proto == CLI_PROTO_JSON) {
		char head[24] = "";

		if (type[0] == 'e') {
			snprintf(head, sizeof(head), ",\"errno\":%d", err);
		}

		cli_print_json(sh, type, head, buf, false);
	} else if (type[0] == 'e') {
		shell_print(sh, "ERR %d%s%s", err, len > 0 ? " " : "", buf);
		if (hint != NULL && proto == CLI_PROTO_HUMAN) {
			print_hint_human(sh, hint);
		}
	} else {
		shell_print(sh, "OK%s%s", len > 0 ? " " : "", buf);
	}

	cli_on_reply(type[0] != 'e');
}

void cli_ok(const struct shell *sh, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprint(sh, "ok", 0, NULL, fmt, ap);
	va_end(ap);
}

int cli_err(const struct shell *sh, int err, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprint(sh, "err", err, NULL, fmt, ap);
	va_end(ap);

	return err;
}

int cli_err_hint(const struct shell *sh, int err, const char *hint, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprint(sh, "err", err, hint, fmt, ap);
	va_end(ap);

	return err;
}

void cli_line(const struct shell *sh, const char *fmt, ...)
{
	char buf[CLI_LINE_MAX];
	va_list ap;

	va_start(ap, fmt);
	cli_vformat_line(buf, fmt, ap);
	va_end(ap);
	if (proto == CLI_PROTO_JSON) {
		cli_print_json(sh, "data", "", buf, true);
	} else {
		shell_print(sh, "%s", buf);
	}
}

const char *cli_str(const char *s, char *buf, size_t len)
{
	size_t o = 0;

	if (s == NULL || s[0] == '\0') {
		return "none";
	}

	if (strpbrk(s, " \"\\=") == NULL || len < 3) {
		return s;
	}

	buf[o++] = '"';
	for (; *s != '\0' && o + 3 < len; s++) {
		if (*s == '"' || *s == '\\') {
			buf[o++] = '\\';
		}

		buf[o++] = *s;
	}

	if (*s != '\0') {
		cli_note_truncated(); /* value longer than the buffer */
	}

	buf[o++] = '"';
	buf[o] = '\0';

	return buf;
}

struct cli_text cli_quote(const char *s)
{
	struct cli_text t;
	const char *q = cli_str(s, t.s, sizeof(t.s));

	if (q != t.s) {
		snprintf(t.s, sizeof(t.s), "%s", q);
	}

	return t;
}

struct cli_text cli_quote_short(const char *s)
{
	char tmp[44];

	if (s != NULL && strlen(s) > 40) {
		memcpy(tmp, s, 37);
		memcpy(&tmp[37], "...", sizeof("..."));
		s = tmp;
	}

	return cli_quote(s);
}
