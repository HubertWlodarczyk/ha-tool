/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Firmware and CLI protocol versions.
 */

#ifndef LEABENCH_VERSION_H_
#define LEABENCH_VERSION_H_

#include <zephyr/sys/util.h>

#define LEABENCH_VERSION "0.2.0"

/* CLI protocol version (protocol/leabench.yaml); bumped on incompatible changes */
#define LEABENCH_PROTO 1

#endif /* LEABENCH_VERSION_H_ */
