#!/usr/bin/env python3
"""Build one isolated firmware profile with an activated ESP-IDF 5.5.4."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


EXPECTED_IDF_VERSION = "ESP-IDF v5.5.4"
PROFILES = ("ble_primary", "legacy_udp")


def idf_command() -> list[str]:
    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        idf_script = Path(idf_path) / "tools" / "idf.py"
    else:
        found = shutil.which("idf.py")
        if not found:
            raise RuntimeError("Activate ESP-IDF 5.5.4 before building (IDF_PATH is unset).")
        idf_script = Path(found)
    if not idf_script.is_file():
        raise RuntimeError("Cannot find idf.py; activate ESP-IDF 5.5.4 first.")

    python_env = os.environ.get("IDF_PYTHON_ENV_PATH")
    if python_env:
        env_root = Path(python_env)
        candidates = (env_root / "Scripts" / "python.exe", env_root / "bin" / "python")
        python = next((path for path in candidates if path.is_file()), None)
        if python is None:
            raise RuntimeError("IDF_PYTHON_ENV_PATH has no Python executable; reactivate ESP-IDF.")
        interpreter = str(python)
    else:
        interpreter = shutil.which("python") or sys.executable
    return [interpreter, str(idf_script)]


def build_args(root: Path, profile: str) -> list[str]:
    build_dir = root / "build" / profile
    defaults = [root / "sdkconfig.defaults", root / f"sdkconfig.{profile}.defaults"]
    if any(not path.is_file() for path in defaults):
        raise RuntimeError(f"Missing sdkconfig defaults for {profile}.")
    # CMake uses semicolons for SDKCONFIG_DEFAULTS on every host OS. Forward
    # arguments directly, without a shell, so Windows never splits this list.
    return [
        "-B", build_dir.as_posix(),
        "-D", f"SDKCONFIG={(build_dir / 'sdkconfig').as_posix()}",
        "-D", f"SDKCONFIG_DEFAULTS={';'.join(path.as_posix() for path in defaults)}",
        "build",
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", nargs="?", choices=PROFILES, default="ble_primary")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    try:
        command = idf_command()
        if os.name == "nt" and any(" " in part or "(" in part or ")" in part
                               for part in (str(root), command[0], command[1])):
            raise RuntimeError("ESP-IDF 5.5 on Windows requires project, IDF, and Python paths without spaces or parentheses.")
        version = subprocess.run(command + ["--version"], cwd=root,
                                 capture_output=True, text=True, check=False)
        if version.returncode != 0:
            raise RuntimeError("Cannot run idf.py; activate ESP-IDF 5.5.4 first.")
        if version.stdout.strip() != EXPECTED_IDF_VERSION:
            raise RuntimeError(f"Expected {EXPECTED_IDF_VERSION}, found {version.stdout.strip() or 'unknown'}.")
        return subprocess.run(command + build_args(root, args.profile), cwd=root,
                              check=False).returncode
    except (OSError, RuntimeError) as exc:
        print(f"Build setup error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
