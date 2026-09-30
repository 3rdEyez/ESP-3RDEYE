#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=$(mktemp)
TMPDIR_BLE=$(mktemp -d)
trap 'rm -f "$OUT"; rm -rf "$TMPDIR_BLE"' EXIT
python3 "$ROOT/tests/ble_core/generate_vector_header.py" \
  "$ROOT/docs/ble/v1/satori_ble_v1_golden_vectors.json" "$TMPDIR_BLE/vector_fixture.hpp" \
  "$ROOT/docs/ble/v1/satori_ble_v1_1_management_vectors.json" \
  "$ROOT/docs/ble/v1/satori_ble_v1_2_shared_pairing_vectors.json"
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -pedantic -I"$ROOT/main/ble" \
  -I"$TMPDIR_BLE" "$ROOT/tests/ble_core/host_test.cpp" "$ROOT/main/ble/ble_protocol.cpp" \
  "$ROOT/main/ble/ble_session.cpp" "$ROOT/main/ble/ble_motion.cpp" -o "$OUT"
"$OUT"
