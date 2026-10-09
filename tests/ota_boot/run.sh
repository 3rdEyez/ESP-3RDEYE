#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -pedantic -I"$ROOT/main/ble" "$ROOT/tests/ota_boot/host_test.cpp" -o "$OUT"
"$OUT"
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -pedantic \
    -I"$ROOT/tests/ota_boot/stubs" -I"$ROOT/main/ble" -I"$ROOT/main/driver/inc" \
    "$ROOT/tests/ota_boot/integration_test.cpp" \
    "$ROOT/main/driver/src/ota_boot_confirm.cpp" -o "$OUT"
"$OUT"
