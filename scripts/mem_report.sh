#!/usr/bin/env bash
# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

# Memory of the nRF5340 DK images for a configuration variant: builds ha_tool (app core) and the
# controller (hci_ipc, net core) with extra Kconfig options and prints RAM / flash of both, plus
# the largest RAM symbols of the net core.
#   scripts/mem_report.sh <name> [hci_ipc_CONFIG_X=v ...] [CONFIG_Y=v ...]
#     hci_ipc_CONFIG_*  net core option (ha_tool/netcore.conf), CONFIG_*  app core option
#   scripts/mem_report.sh base                      # the configuration as committed
#   scripts/mem_report.sh iso17 hci_ipc_CONFIG_BT_CTLR_ISO_TX_BUFFERS=17 \
#     hci_ipc_CONFIG_BT_ISO_TX_BUF_COUNT=17 CONFIG_BT_ISO_TX_BUF_COUNT=17
# Results: build/mem/<name>.txt (top symbols in build/mem/<name>.net_ram.txt).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="${1:?usage: $0 <name> [CONFIG options]}"
shift
BUILD="$ROOT/build/mem_$NAME"
OUT="$ROOT/build/mem"
mkdir -p "$OUT"

DEFS=()
for opt in "$@"; do
  DEFS+=("-D$opt")
done

west build -b nrf5340dk/nrf5340/cpuapp --sysbuild -p auto -d "$BUILD" "$ROOT/ha_tool" \
  -- "${DEFS[@]}" > "$OUT/$NAME.build.log" 2>&1 || {
  tail -30 "$OUT/$NAME.build.log"
  exit 1
}

ninja -C "$BUILD/hci_ipc" ram_report > "$OUT/$NAME.net_ram.raw" 2>&1
{
  echo "variant: $NAME $*"
  grep -E "^ +(FLASH|RAM):" "$OUT/$NAME.build.log" | sed -n '1s/^/net /p;2s/^/net /p;3s/^/app /p;4s/^/app /p'
} | tee "$OUT/$NAME.txt"

# Largest net core RAM symbols
python3 - "$OUT/$NAME.net_ram.raw" > "$OUT/$NAME.net_ram.txt" <<'EOF'
import re
import sys

rows = []
for line in open(sys.argv[1]):
    m = re.match(r"^[\s│├└─]*([\w.\-\[\]]+)\s+(\d+)\s+[\d.]+%\s+(0x[0-9a-f]+)\s+(\w+)", line)
    if m:
        rows.append((int(m.group(2)), m.group(1)))
for size, name in sorted(rows, reverse=True)[:30]:
    print(f"{size:7d}  {name}")
EOF
