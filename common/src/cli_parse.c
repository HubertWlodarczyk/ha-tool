/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/* CLI input: key=value arguments, addresses, enums. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/shell/shell.h>

#include "cli.h"

const char *cli_kv(size_t argc, char **argv, const char *key)
{
	const size_t key_len = strlen(key);

	for (size_t i = 1; i < argc; i++) {
		if (strncmp(argv[i], key, key_len) == 0 && argv[i][key_len] == '=') {
			return &argv[i][key_len + 1];
		}
	}

	return NULL;
}

long cli_kv_long(size_t argc, char **argv, const char *key, long def, int *err)
{
	const char *val = cli_kv(argc, argv, key);
	char *end;
	long ret;

	if (val == NULL) {
		return def;
	}

	ret = strtol(val, &end, 0);
	if (end == val || *end != '\0') {
		*err = -EINVAL;
		return def;
	}

	return ret;
}

int cli_parse_addr(const char *str, bt_addr_le_t *addr)
{
	char tmp[BT_ADDR_STR_LEN];
	const char *sep = strchr(str, '/');
	size_t len = sep != NULL ? (size_t)(sep - str) : strlen(str);

	if (len != BT_ADDR_STR_LEN - 1) {
		return -EINVAL;
	}

	memcpy(tmp, str, len);
	tmp[len] = '\0';

	if (sep == NULL || strcmp(sep + 1, "random") == 0) {
		addr->type = BT_ADDR_LE_RANDOM;
	} else if (strcmp(sep + 1, "public") == 0) {
		addr->type = BT_ADDR_LE_PUBLIC;
	} else {
		return -EINVAL;
	}

	return bt_addr_from_str(tmp, &addr->a);
}

void cli_addr_str(const bt_addr_le_t *addr, char *buf, size_t len)
{
	char a[BT_ADDR_STR_LEN];

	bt_addr_to_str(&addr->a, a, sizeof(a));
	snprintf(buf, len, "%s/%s", a, addr->type == BT_ADDR_LE_PUBLIC ? "public" : "random");
}

int cli_parse_u8(const char *str, uint8_t *out)
{
	char *end;
	unsigned long v = strtoul(str, &end, 0);

	if (end == str || *end != '\0' || v > UINT8_MAX) {
		return -EINVAL;
	}

	*out = (uint8_t)v;

	return 0;
}

const char *cli_hci_reason_str(uint8_t reason)
{
	switch (reason) {
	case BT_HCI_ERR_SUCCESS:
		return "success";
	case BT_HCI_ERR_AUTH_FAIL:
		return "auth_fail";
	case BT_HCI_ERR_PIN_OR_KEY_MISSING:
		return "key_missing";
	case BT_HCI_ERR_CONN_TIMEOUT:
		return "supervision_timeout";
	case BT_HCI_ERR_REMOTE_USER_TERM_CONN:
		return "remote_user_term";
	case BT_HCI_ERR_LOCALHOST_TERM_CONN:
		return "local_term";
	case BT_HCI_ERR_UNKNOWN_CONN_ID:
		return "unknown_conn";
	case BT_HCI_ERR_CONN_FAIL_TO_ESTAB:
		return "fail_to_establish";
	case BT_HCI_ERR_UNSPECIFIED:
		return "unspecified";
	case BT_HCI_ERR_INSUFFICIENT_RESOURCES:
		return "no_resources";
	case BT_HCI_ERR_PAIRING_NOT_SUPPORTED:
		return "pairing_not_supported";
	default:
		return "other";
	}
}

int cli_parse_enum(const char *str, const char *const *names, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (names[i] != NULL && strcmp(str, names[i]) == 0) {
			return (int)i;
		}
	}

	return -EINVAL;
}

const char *cli_fmt_fixed(int32_t value, unsigned int scale, char *buf, size_t len)
{
	/* value / scale with one decimal, rounded half away from zero */
	const int64_t tenths =
		((int64_t)value * 10 + (value < 0 ? -(int64_t)scale : (int64_t)scale) / 2) /
		(int64_t)scale;
	const int64_t mag = tenths < 0 ? -tenths : tenths;

	snprintf(buf, len, "%s%lld.%lld", tenths < 0 ? "-" : "", (long long)(mag / 10),
		 (long long)(mag % 10));

	return buf;
}

struct cli_num cli_fixed(int32_t value, unsigned int scale)
{
	struct cli_num n;

	cli_fmt_fixed(value, scale, n.s, sizeof(n.s));

	return n;
}
