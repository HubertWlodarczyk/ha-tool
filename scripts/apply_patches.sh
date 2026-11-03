#!/usr/bin/env bash
# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

# Applies the Zephyr patches needed by leabench to the west workspace (idempotent).
#   scripts/apply_patches.sh [path/to/zephyr]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ZEPHYR="${1:-${ZEPHYR_BASE:?set ZEPHYR_BASE or pass the zephyr directory}}"

for patch in "$ROOT"/zephyr/patches/zephyr/*.patch; do
  if git -C "$ZEPHYR" apply --reverse --check "$patch" 2>/dev/null; then
    echo "already applied: $(basename "$patch")"
  else
    git -C "$ZEPHYR" apply "$patch"
    echo "applied: $(basename "$patch")"
  fi
done
