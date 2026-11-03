#!/usr/bin/env bash
# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

# Runs the simulation test suite (build first with scripts/build.sh).
#   scripts/test.sh                      all tests
#   scripts/test.sh -k phone             subset
#   LEA_SIM_SPEED=2 scripts/test.sh       simulate 2x faster than real time
set -euo pipefail
cd "$(dirname "$0")/../tests"
exec python3 -m pytest "$@"
