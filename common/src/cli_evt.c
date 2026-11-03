/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Asynchronous events: formatted into a ring buffer and printed by a thread, so Bluetooth callbacks
 * never block on the UART. Events can be filtered by name.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>

#include "cli.h"
#include "cli_internal.h"

#define EVT_HEAD_MAX   (sizeof("EVT t=18446744073709551615  ") + EVT_NAME_MAX)
#define EVT_LINE_MAX   (EVT_HEAD_MAX + CLI_LINE_MAX)
/* Pending events as length-prefixed text: ~50 typical events (a fixed slot per event, sized
 * for the longest line, took 3x the RAM for 32)
 */
#define EVT_BUF_SIZE   4096
#define EVT_FILTER_MAX 16
#define EVT_NAME_MAX   24

struct evt_line {
	char text[EVT_LINE_MAX];
};

BUILD_ASSERT(EVT_LINE_MAX <= UINT16_MAX, "record length is 16 bit");
BUILD_ASSERT(EVT_BUF_SIZE >= 4 * (EVT_LINE_MAX + sizeof(uint16_t)), "room for long events");

/* cli_evt() may run in any thread or ISR: records go in and out under evt_lock */
RING_BUF_DECLARE(evt_rb, EVT_BUF_SIZE);
static struct k_spinlock evt_lock;
static K_SEM_DEFINE(evt_count, 0, K_SEM_MAX_LIMIT); /* records in evt_rb */
static atomic_t evt_dropped;                        /* since the last report line */
static atomic_t evt_dropped_total;                  /* since boot */
static char evt_filter[EVT_FILTER_MAX][EVT_NAME_MAX];
static K_MUTEX_DEFINE(filter_lock);

static bool evt_filtered(const char *name)
{
	bool hit = false;

	k_mutex_lock(&filter_lock, K_FOREVER);
	for (int i = 0; i < EVT_FILTER_MAX && !hit; i++) {
		hit = evt_filter[i][0] != '\0' && strcmp(evt_filter[i], name) == 0;
	}
	k_mutex_unlock(&filter_lock);

	return hit;
}

int cli_evt_filter(const char *name, bool off)
{
	int ret = off ? -ENOMEM : 0;

	if (strlen(name) >= EVT_NAME_MAX) {
		return -EINVAL;
	}

	k_mutex_lock(&filter_lock, K_FOREVER);
	for (int i = 0; i < EVT_FILTER_MAX; i++) {
		if (strcmp(evt_filter[i], name) == 0) {
			if (!off) {
				evt_filter[i][0] = '\0';
			}

			ret = 0;
			off = false; /* already filtered */
			break;
		}
	}

	for (int i = 0; off && i < EVT_FILTER_MAX; i++) {
		if (evt_filter[i][0] == '\0') {
			memcpy(evt_filter[i], name, strlen(name) + 1); /* length checked above */
			ret = 0;
			break;
		}
	}
	k_mutex_unlock(&filter_lock);

	return ret;
}

void cli_evt_filter_clear(void)
{
	k_mutex_lock(&filter_lock, K_FOREVER);
	memset(evt_filter, 0, sizeof(evt_filter));
	k_mutex_unlock(&filter_lock);
}

size_t cli_evt_filter_list(char *buf, size_t len)
{
	size_t pos = 0;

	buf[0] = '\0';
	k_mutex_lock(&filter_lock, K_FOREVER);
	for (int i = 0; i < EVT_FILTER_MAX; i++) {
		if (evt_filter[i][0] != '\0' && pos + EVT_NAME_MAX + 2 < len) {
			pos += snprintf(&buf[pos], len - pos, "%s%s", pos > 0 ? "," : "",
					evt_filter[i]);
		}
	}
	k_mutex_unlock(&filter_lock);

	return pos;
}

/* Formats one event into text (body is scratch) and queues it */
static void evt_queue(char *body, char *text, size_t text_len, const char *name, const char *fmt,
		      va_list ap)
{
	bool queued = false;
	uint16_t n;
	int len;

	len = cli_vformat_line(body, fmt, ap);
	snprintf(text, text_len, "EVT t=%lld %.*s%s%s", k_uptime_get(), EVT_NAME_MAX, name,
		 len > 0 ? " " : "", body);

	n = strlen(text);
	K_SPINLOCK(&evt_lock) {
		if (ring_buf_space_get(&evt_rb) >= sizeof(n) + n) {
			ring_buf_put(&evt_rb, (const uint8_t *)&n, sizeof(n));
			ring_buf_put(&evt_rb, (const uint8_t *)text, n);
			queued = true;
		}
	}

	if (queued) {
		k_sem_give(&evt_count);
	} else {
		atomic_inc(&evt_dropped);
		atomic_inc(&evt_dropped_total);
	}
}

/* From an ISR (no mutex): the buffers go on the interrupt stack, kept out of cli_evt's frame */
static __noinline void evt_queue_isr(const char *name, const char *fmt, va_list ap)
{
	char body[CLI_LINE_MAX];
	char text[EVT_LINE_MAX];

	evt_queue(body, text, sizeof(text), name, fmt, ap);
}

/*
 * Threads format into static buffers under a mutex: cli_evt is called from Bluetooth callbacks
 * deep in the host's RX thread, where ~700 octets of line buffers on the caller's stack were
 * what overflowed it on the board (the simulation runs threads on host stacks and cannot see
 * it; scripts/stack_report.py).
 */
static K_MUTEX_DEFINE(evt_fmt_lock);
static char evt_body[CLI_LINE_MAX];
static char evt_text[EVT_LINE_MAX];

void cli_evt(const char *name, const char *fmt, ...)
{
	va_list ap;

	if (evt_filtered(name)) {
		return;
	}

	va_start(ap, fmt);
	if (k_is_in_isr()) {
		evt_queue_isr(name, fmt, ap);
	} else {
		k_mutex_lock(&evt_fmt_lock, K_FOREVER);
		evt_queue(evt_body, evt_text, sizeof(evt_text), name, fmt, ap);
		k_mutex_unlock(&evt_fmt_lock);
	}
	va_end(ap);
}

void cli_evt_stats_get(struct cli_stats *st)
{
	st->evt_dropped = atomic_get(&evt_dropped_total);
}

__weak bool cli_evt_human(const char *name, const char *body, char *out, size_t len)
{
	ARG_UNUSED(name);
	ARG_UNUSED(body);
	ARG_UNUSED(out);
	ARG_UNUSED(len);

	return false;
}

bool cli_body_get(const char *body, const char *key, char *out, size_t len)
{
	const size_t klen = strlen(key);
	const char *p = body;

	while (p != NULL && *p != '\0') {
		while (*p == ' ') {
			p++;
		}

		if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
			const char *v = p + klen + 1;
			size_t o = 0;

			if (*v == '"') { /* quoted (cli_str): until the closing quote, \ escapes */
				for (v++; *v != '\0' && *v != '"' && o + 1 < len; v++) {
					if (*v == '\\' && v[1] != '\0') {
						v++;
					}

					out[o++] = *v;
				}
			} else {
				for (; *v != '\0' && *v != ' ' && o + 1 < len; v++) {
					out[o++] = *v;
				}
			}

			out[o] = '\0';
			return true;
		}

		/* next word (a quoted value may contain spaces) */
		bool quoted = false;

		for (; *p != '\0' && (quoted || *p != ' '); p++) {
			if (*p == '"') {
				quoted = !quoted;
			} else if (*p == '\\' && quoted && p[1] != '\0') {
				p++;
			}
		}
	}

	if (len > 0) {
		out[0] = '\0';
	}

	return false;
}

/* "EVT t=<ms> <name> k=v ..." in the current protocol (human: a sentence if there is one) */
static void print_evt(const struct shell *sh, const char *text)
{
	char head[EVT_NAME_MAX + 48];
	const char *t = text + 4;
	const char *name;
	const char *body;
	long long ms;

	ms = strtoll(t + 2, (char **)&name, 10);
	name++;
	body = strchr(name, ' ');

	if (cli_proto_get() == CLI_PROTO_HUMAN) {
		char evt_name[EVT_NAME_MAX + 1];
		/* Only the event thread prints: static, not on its stack */
		static char sentence[CLI_LINE_MAX];
		const size_t n =
			MIN(body != NULL ? (size_t)(body - name) : strlen(name), EVT_NAME_MAX);

		memcpy(evt_name, name, n);
		evt_name[n] = '\0';
		sentence[0] = '\0';
		if (cli_evt_human(evt_name, body != NULL ? body + 1 : "", sentence,
				  sizeof(sentence))) {
			if (sentence[0] != '\0') {
				shell_print(sh, "%s", sentence);
			}
			if (!cli_human_verbose()) {
				return;
			}
		}
	}

	if (cli_proto_get() != CLI_PROTO_JSON) {
		shell_print(sh, "%s", text);
		return;
	}

	snprintf(head, sizeof(head), ",\"t\":%lld,\"evt\":\"%.*s\"", ms,
		 (int)MIN(body != NULL ? (size_t)(body - name) : strlen(name), EVT_NAME_MAX), name);
	cli_print_json(sh, "evt", head, body != NULL ? body + 1 : "", false);
}

static void evt_thread(void *p1, void *p2, void *p3)
{
	static struct evt_line line; /* this thread only: off its stack */

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		const struct shell *sh;
		atomic_val_t dropped;
		uint16_t n = 0;
		char text[48];

		k_sem_take(&evt_count, K_FOREVER);
		K_SPINLOCK(&evt_lock) {
			(void)ring_buf_get(&evt_rb, (uint8_t *)&n, sizeof(n));
			(void)ring_buf_get(&evt_rb, (uint8_t *)line.text, n); /* n < EVT_LINE_MAX */
		}
		line.text[n] = '\0';
		sh = shell_backend_uart_get_ptr();
		print_evt(sh, line.text);

		dropped = atomic_set(&evt_dropped, 0);
		if (dropped > 0) {
			snprintf(text, sizeof(text), "EVT t=%lld dropped n=%ld", k_uptime_get(),
				 (long)dropped);
			print_evt(sh, text);
		}
	}
}

/* print_evt -> shell -> UART transport: 1.8 KB worst case by scripts/stack_report.py, plus the
 * shell's calls through pointers it cannot follow
 */
K_THREAD_DEFINE(cli_evt_tid, 3072, evt_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(10), 0, 0);
