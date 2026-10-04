#!/usr/bin/env bash
set -euo pipefail
# Historical fixed-offset partition reads are unsafe across partition migrations.
# Keep this entry discoverable, but never open/reset a serial device implicitly.
echo 'Deprecated fixed-offset backup helper; no device was accessed.'
echo 'Use tools/satori_dev.py usb-upgrade --help for validated full double backups and upgrade recovery.'
exit 2
