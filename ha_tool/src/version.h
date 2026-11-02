/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Firmware and CLI protocol versions.
 */

#ifndef HA_TOOL_VERSION_H_
#define HA_TOOL_VERSION_H_

#include <zephyr/sys/util.h>

#define HA_TOOL_VERSION "0.2.0"

/* CLI protocol version (protocol/hatool.yaml); bumped on incompatible changes */
#define HA_TOOL_PROTO 1

#endif /* HA_TOOL_VERSION_H_ */
