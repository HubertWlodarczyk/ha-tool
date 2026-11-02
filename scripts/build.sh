#!/usr/bin/env bash
# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

# Builds ha_tool and dut_sim.
#   scripts/build.sh            simulation (nrf5340bsim), output in build/<app>
#   scripts/build.sh dk         nRF5340 DK (needs the Zephyr SDK), output in build/<app>_dk
#   scripts/build.sh sim -p     pristine rebuild
#   scripts/build.sh sim --sanitize   ASan + UBSan, output in build/<app>_asan
#   scripts/build.sh sim --coverage   gcov, output in build/<app>_cov
#   scripts/build.sh dk --diag        stack measurement shell commands (ha_tool/diag.conf),
#                                     output in build/<app>_dk_diag
# Tests use a variant with HA_VARIANT=asan|cov scripts/test.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="${1:-sim}"
shift || true

VARIANT=""
VARIANT_CONF=()
ARGS=()
for a in "$@"; do
  case "$a" in
    --sanitize) VARIANT=_asan; VARIANT_CONF=(-DCONFIG_ASAN=y -DCONFIG_UBSAN=y) ;;
    --coverage) VARIANT=_cov;  VARIANT_CONF=(-DCONFIG_COVERAGE=y) ;;
    --diag)     VARIANT=_diag; VARIANT_CONF=(-DEXTRA_CONF_FILE=diag.conf) ;;
    *) ARGS+=("$a") ;;
  esac
done

case "$TARGET" in
  sim) BOARD=nrf5340bsim/nrf5340/cpuapp; SUFFIX=""; EXTRA=(-DZEPHYR_TOOLCHAIN_VARIANT=host) ;;
  dk)  BOARD=nrf5340dk/nrf5340/cpuapp;   SUFFIX="_dk"; EXTRA=() ;;
  *)   echo "usage: $0 [sim|dk] [--sanitize|--coverage] [west build options]"; exit 1 ;;
esac
if [ "$VARIANT" = _asan ] || [ "$VARIANT" = _cov ]; then
  [ "$TARGET" = sim ] || { echo "--sanitize / --coverage: simulation only"; exit 1; }
fi

APPS=(ha_tool)
[ "$TARGET" = sim ] && APPS+=(dut_sim)

for app in "${APPS[@]}"; do
  echo "=== $app ($BOARD)"
  west build "${ARGS[@]}" -b "$BOARD" --sysbuild -d "$ROOT/build/$app$SUFFIX$VARIANT" "$ROOT/$app" \
    -- "${EXTRA[@]}" "${VARIANT_CONF[@]}"
done
