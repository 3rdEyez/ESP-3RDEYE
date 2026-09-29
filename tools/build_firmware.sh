#!/usr/bin/env bash
set -euo pipefail

profile="${1:-}"
case "$profile" in
  ble_primary|legacy_udp) ;;
  *) echo "Usage: $0 ble_primary|legacy_udp" >&2; exit 2 ;;
esac

if ! command -v idf.py >/dev/null 2>&1; then
  echo "idf.py is not on PATH; activate ESP-IDF 5.5.4 first." >&2
  exit 2
fi
idf_version="$(idf.py --version)"
if [[ "$idf_version" != "ESP-IDF v5.5.4" ]]; then
  echo "Expected ESP-IDF v5.5.4, found: $idf_version" >&2
  exit 2
fi

project_root="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$project_root/build/$profile"
defaults="$project_root/sdkconfig.defaults;$project_root/sdkconfig.$profile.defaults"
sdkconfig="$build_dir/sdkconfig"

# The profile-local ignored sdkconfig prevents a developer's root sdkconfig
# (which may contain Wi-Fi credentials) from entering either artifact.
exec idf.py -B "$build_dir" \
  -D "SDKCONFIG=$sdkconfig" \
  -D "SDKCONFIG_DEFAULTS=$defaults" \
  build
