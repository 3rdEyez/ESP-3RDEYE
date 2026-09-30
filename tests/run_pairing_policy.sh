#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -pedantic \
  "$ROOT/tests/pairing_policy_test.cpp" "$ROOT/main/ble/ble_pairing.cpp" -o "$OUT"
"$OUT"
