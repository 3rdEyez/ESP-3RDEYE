#!/usr/bin/env python3
"""Explicit migration entry; default is offline plan. See tools/README.md."""
import sys
from satori_dev import main

if __name__ == '__main__':
    raise SystemExit(main(['migrate-layout', *sys.argv[1:]]))
