#!/usr/bin/env bash
# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

# Run a small broadcast scenario using the raw CLI.
set -euo pipefail

PORT="${1:?usage: $0 PORT}"

python3 scripts/cli.py --port "$PORT" \
  "mode broadcast" \
  "broadcast 1 create name=Example preset=24_2 bis=1" \
  "broadcast 1 bis tone 1 freq=1000 level=-20" \
  "broadcast 1 start" \
  "broadcast 1 stop"
