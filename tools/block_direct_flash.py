#!/usr/bin/env python3
"""Build-system backstop only; no device or SDK imports."""
import sys
print('Direct SDK flash targets are disabled. Use checked usb-upgrade or explicit migrate_layout.py; no automatic migration.', file=sys.stderr)
raise SystemExit(2)
