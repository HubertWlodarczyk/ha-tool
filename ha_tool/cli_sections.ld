/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include <zephyr/linker/iterable_sections.h>

/* CLI_KEYS() sets (common/include/cli.h) */
ITERABLE_SECTION_ROM(cli_keyset, Z_LINK_ITERABLE_SUBALIGN)
