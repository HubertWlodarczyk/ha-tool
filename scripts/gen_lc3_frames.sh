#!/usr/bin/env bash
# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

# Regenerates ha_tool/src/lc3_frames.c (precomputed LC3 sine frames) with the host compiler and
# the liblc3 module of the Zephyr workspace. Deterministic: run twice, same file.
#   scripts/gen_lc3_frames.sh [--max-cycle 1|2|4]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WS="${ZEPHYR_WORKSPACE:-$HOME/zephyrproject}"
LC3="$WS/modules/lib/liblc3"
BUILD="$ROOT/build/lc3gen"

mkdir -p "$BUILD"
gcc -O2 -Wall -I"$LC3/include" "$LC3"/src/*.c "$ROOT/tools/lc3gen/lc3gen.c" -lm \
	-o "$BUILD/lc3gen"
"$BUILD/lc3gen" "$@" > "$BUILD/lc3_frames.c"
mv "$BUILD/lc3_frames.c" "$ROOT/ha_tool/src/lc3_frames.c"
echo "ha_tool/src/lc3_frames.c updated"
