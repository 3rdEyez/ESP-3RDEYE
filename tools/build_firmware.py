#!/usr/bin/env python3
"""Build unsigned dual-OTA by default with ESP-IDF 5.5.4; factory profiles are explicit historical choices."""

import argparse
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys


EXPECTED_IDF_VERSION = "ESP-IDF v5.5.4"
PROFILES = ("ble_primary", "legacy_udp", "ble_dual_ota", "ble_wifi_ota_prototype")


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


def main_profile(profile: str) -> int:
    if profile not in PROFILES:
        raise ValueError("Unknown firmware profile")
    root = Path(__file__).resolve().parent.parent
    try:
        cached = root / 'build' / profile / 'sdkconfig'
        if cached.is_file() and any(line.strip() == 'CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y' for line in cached.read_text().splitlines()):
            raise RuntimeError('Automatic signing is not a build action; use explicit sign-package')
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
        result = subprocess.run(command + build_args(root, profile), cwd=root, check=False, capture_output=True, text=True)
        # SDK boilerplate recommends unchecked full flash; product updates use our gated tools.
        print(result.stdout.split('Project build complete.', 1)[0], end='')
        if result.stderr: print(result.stderr, end='', file=sys.stderr)
        code = result.returncode
        if code == 0:
            version = (root / 'version.txt').read_text().strip()
            if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', version):
                raise RuntimeError('Invalid artifact version')
            image = root / 'build' / profile / 'app.bin'
            if image.is_file():
                from satori_tools.ota_records import classify_table
                actual_layout = classify_table((image.parent / 'partition_table/partition-table.bin').read_bytes())
                expected_layout = 'dual-ota' if profile in ('ble_dual_ota', 'ble_wifi_ota_prototype') else 'factory'
                if actual_layout != expected_layout:
                    raise RuntimeError('Cached layout conflicts with profile; reconfigure the isolated build')
                layout = 'dual-ota' if expected_layout == 'dual-ota' else 'factory-historical'
                artifact = image.with_name(f'satori-{version}-{profile}-{layout}-unsigned.bin')
                if profile == 'ble_wifi_ota_prototype':
                    from satori_tools.migration import build_proof, REQUIRED_FLAGS
                    proof = build_proof(image.parent, EXPECTED_IDF_VERSION)
                    if proof['flags'] != REQUIRED_FLAGS:
                        raise RuntimeError('Cached security/profile flags conflict; no deployable artifact proof')
                    artifact.with_suffix('.public-build.json').write_text(json.dumps(proof, indent=2) + '\n')
                shutil.copyfile(image, artifact)
        if code == 0: print('Build complete: unsigned development artifact; use explicit migration or checked update tools, never direct flash.')
        return code
    except (OSError, RuntimeError) as exc:
        print(f"Build setup error: {exc}", file=sys.stderr)
        return 2


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", nargs="?", choices=PROFILES, default="ble_wifi_ota_prototype", help="default: ble_wifi_ota_prototype (dual OTA); factory profiles require explicit selection")
    return main_profile(parser.parse_args().profile)


if __name__ == "__main__":
    sys.exit(main())
