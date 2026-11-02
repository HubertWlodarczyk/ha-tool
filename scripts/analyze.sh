#!/usr/bin/env bash
# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

# Static analysis of the project's C sources: gcc -fanalyzer and clang-tidy (.clang-tidy).
# Needs the simulation builds (scripts/build.sh).
#   scripts/analyze.sh                    everything
#   scripts/analyze.sh --only tidy        one tool
#   scripts/analyze.sh ha_tool/src/dev.c  some files
set -euo pipefail
exec python3 "$(dirname "$0")/../tools/analyze/analyze.py" "$@"
