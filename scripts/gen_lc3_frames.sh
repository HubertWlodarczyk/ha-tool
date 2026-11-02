#!/usr/bin/env bash
# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

# Regenerates ha_tool/src/lc3_frames.c (precomputed LC3 sine frames) with the host compiler and
# the liblc3 module of the Zephyr workspace. Deterministic: run twice, same file.
#   scripts/gen_lc3_frames.sh [--max-cycle 1|2|4]
#
# --probe measures the tone quality of the current table instead (tools/lc3gen/lc3probe.c):
# SNR, spurious lines, envelope and peak of every tone at several levels, next to a continuous
# encoding of the same tone, in build/lc3probe/report.csv. With `wav` it writes listening samples:
#   scripts/gen_lc3_frames.sh --probe [wav <out.wav> <cfg> <Hz> <dBFS> [clean|loss|phase|ref]]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WS="${ZEPHYR_WORKSPACE:-$HOME/zephyrproject}"
LC3="$WS/modules/lib/liblc3"

if [ "${1:-}" = "--probe" ]; then
	shift
	BUILD="$ROOT/build/lc3probe"
	mkdir -p "$BUILD/shim/zephyr"
	# lc3_frames.c is firmware source: BUILD_ASSERT from a stand-in for the Zephyr header
	echo '#define BUILD_ASSERT(c, m) _Static_assert(c, m)' > "$BUILD/shim/zephyr/toolchain.h"
	gcc -O2 -Wall -I"$LC3/include" -I"$ROOT/ha_tool/src" -I"$BUILD/shim" "$LC3"/src/*.c \
		"$ROOT/ha_tool/src/lc3_frames.c" "$ROOT/tools/lc3gen/lc3probe.c" -lm -o "$BUILD/lc3probe"
	if [ $# -eq 0 ]; then
		set -- report "$BUILD/report.csv"
	fi
	exec "$BUILD/lc3probe" "$@"
fi

BUILD="$ROOT/build/lc3gen"
mkdir -p "$BUILD"
gcc -O2 -Wall -I"$LC3/include" "$LC3"/src/*.c "$ROOT/tools/lc3gen/lc3gen.c" -lm \
	-o "$BUILD/lc3gen"
"$BUILD/lc3gen" "$@" > "$BUILD/lc3_frames.c"
mv "$BUILD/lc3_frames.c" "$ROOT/ha_tool/src/lc3_frames.c"
echo "ha_tool/src/lc3_frames.c updated"
