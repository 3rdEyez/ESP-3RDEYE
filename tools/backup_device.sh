#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "Usage: $0 SERIAL_PORT PRIVATE_OUTPUT_DIRECTORY" >&2
  exit 2
fi
port="$1"
output_dir="$(mkdir -p "$2" && cd "$2" && pwd)"
project_root="$(cd "$(dirname "$0")/.." && pwd)"
case "$output_dir/" in
  "$project_root/"*)
    echo "Choose a private directory outside the repository; backups can contain credentials and BLE bond data." >&2
    exit 2
    ;;
esac

python="${IDF_PYTHON_ENV_PATH:-}/bin/python"
if [[ ! -x "$python" ]]; then
  echo "Set IDF_PYTHON_ENV_PATH to the ESP-IDF 5.5.4 Python environment." >&2
  exit 2
fi

umask 077
chmod 700 "$output_dir"
if [[ -e "$output_dir/default-nvs.bin" || -e "$output_dir/board-config.bin" ]]; then
  echo "Backup file already exists; choose a new output directory to avoid overwriting a recovery copy." >&2
  exit 2
fi
"$python" -m esptool --chip esp32c3 --port "$port" --after no_reset read_flash 0x9000 0x6000 "$output_dir/default-nvs.bin"
"$python" -m esptool --chip esp32c3 --port "$port" --after no_reset read_flash 0x300000 0x2800 "$output_dir/board-config.bin"
chmod 600 "$output_dir/default-nvs.bin" "$output_dir/board-config.bin"
echo "Private partition backups written to: $output_dir"
