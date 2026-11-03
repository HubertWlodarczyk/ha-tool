#!/usr/bin/env bash
# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

# Style and consistency checks of the whole repository; exit code 1 on any finding.
#   scripts/lint.sh          check
#   scripts/lint.sh --fix    apply clang-format and ruff fixes first
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ZEPHYR="${ZEPHYR_BASE:-$HOME/zephyrproject/zephyr}"
cd "$ROOT"
status=0
fail() { echo "lint: $1"; status=1; }

mapfile -t C_FILES < <(git ls-files '*.c' '*.h' | grep -v -e '^leabench/src/lc3_frames.c$')
mapfile -t PY_FILES < <(git ls-files '*.py' 'scripts/*.py')

if [ "${1:-}" = "--fix" ]; then
  clang-format -i "${C_FILES[@]}"
  ruff check --fix -q . || true
  ruff format -q .
fi

echo "== clang-format"
clang-format --dry-run -Werror "${C_FILES[@]}" 2>&1 | head -40 || true
clang-format --dry-run -Werror "${C_FILES[@]}" >/dev/null 2>&1 || fail "clang-format (run scripts/lint.sh --fix)"

echo "== checkpatch"
for f in "${C_FILES[@]}"; do
  out=$(perl "$ZEPHYR/scripts/checkpatch.pl" --no-tree --quiet --terse \
        --typedefsfile "$ZEPHYR/scripts/checkpatch/typedefsfile" -f "$f" 2>&1 | grep -E "WARNING|ERROR")
  if [ -n "$out" ]; then echo "$out"; fail "checkpatch $f"; fi
done

echo "== license headers"
for f in "${C_FILES[@]}" "${PY_FILES[@]}" $(git ls-files 'clients/java/src/*.java' '*.sh' '*.ld'); do
  grep -q "SPDX-License-Identifier: MPL-2.0" "$f" || fail "no SPDX header: $f"
done

echo "== ruff"
ruff check -q . || fail "ruff check"
ruff format --check -q . || fail "ruff format (run scripts/lint.sh --fix)"

echo "== generated files"
python3 tools/protogen/gen.py --check || fail "protocol outputs out of date (tools/protogen/gen.py)"

echo "== python client unit tests"
(cd clients/python && python3 -m pytest -q -p no:cacheprovider tests >/dev/null) || fail "client tests (pytest clients/python/tests)"

echo "== java"
if [ -n "${JAVA_HOME:-}" ] || command -v java >/dev/null; then
  (cd clients/java && ./gradlew -q check) || fail "gradle check (checkstyle, JUnit)"
else
  echo "skipped (no JDK)"
fi

[ $status -eq 0 ] && echo "lint: clean"
exit $status
