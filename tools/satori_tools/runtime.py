import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


class Rejected(Exception):
    """Messages are fixed by our code; never include transport/secret data."""


def digest(data):
    return hashlib.sha256(data).hexdigest()


def sdk_command(tool):
    """Use an activated SDK, never a workstation-specific installation."""
    environment = os.environ.get('IDF_PYTHON_ENV_PATH')
    python = str(Path(environment) / ('Scripts/python.exe' if os.name == 'nt' else 'bin/python')) if environment else sys.executable
    idf = os.environ.get('IDF_PATH')
    if idf:
        relative = {'espsecure': 'components/esptool_py/esptool/espsecure.py', 'idf': 'tools/idf.py'}[tool]
        script = Path(idf) / relative
        if not script.is_file():
            raise Rejected('Requested SDK tool is unavailable')
        return [python, str(script)]
    if tool == 'espsecure':
        return [python, '-m', 'espsecure']
    script = shutil.which('idf.py')
    if not script:
        raise Rejected('Activate ESP-IDF before building')
    return [python, script]


def checked(command, timeout=30):
    # Child output may include key paths, serial data or user input.
    result = subprocess.run(command, capture_output=True, timeout=timeout, check=False)
    if result.returncode:
        raise Rejected('External tool failed; output withheld')
    return result


def private_output(path):
    """Raw device snapshots must remain outside this checkout."""
    path = Path(path).resolve()
    if path == ROOT or ROOT in path.parents or any((parent / '.git').exists() for parent in (path, *path.parents)):
        raise Rejected('Choose a private output directory outside the checkout')
    return path
