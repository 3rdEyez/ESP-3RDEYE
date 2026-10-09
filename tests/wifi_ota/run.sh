#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -pedantic \
    -I"$ROOT/main/ota" "$ROOT/tests/wifi_ota/core_test.cpp" -o "$OUT"
"$OUT"
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -pedantic \
    -I"$ROOT/main/ota" -I"$ROOT/main/ble" "$ROOT/tests/wifi_ota/window_protocol_test.cpp" -o "$OUT"
"$OUT"
for policy in 0 1; do
    ${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -pedantic -DTEST_NATIVE_POLICY="$policy" \
        -I"$ROOT/tests/wifi_ota/stubs" -I"$ROOT/main/ota" \
        "$ROOT/tests/wifi_ota/adapter_test.cpp" "$ROOT/main/ota/ota_idf_sink.cpp" -o "$OUT"
    "$OUT"
done
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -pedantic \
    -I"$ROOT/main/ota" "$ROOT/tests/wifi_ota/lan_protocol_test.cpp" -o "$OUT"
"$OUT"
