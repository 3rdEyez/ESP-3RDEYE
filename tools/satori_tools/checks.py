"""One offline check runner. No signing, installation, flashing or provisioning."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
from .runtime import ROOT, Rejected

HOST_SCRIPTS = ('run_config_value.sh', 'run_legacy_target.sh', 'run_mechanical_mapping.sh',
                'run_pairing_policy.sh', 'run_startup_profile.sh', 'ble_core/run.sh',
                'ble_core/run_pairing.sh', 'ota_boot/run.sh', 'wifi_ota/run.sh')


def run(app_path=None, flutter=None):
    if not os.environ.get('IDF_PATH'):
        raise Rejected('Set IDF_PATH: native HTTP checks must not silently skip')
    commands = [[sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_*.py']]
    commands.extend([['sh', 'tests/' + name] for name in HOST_SCRIPTS])
    for command in commands:
        # Test inputs are synthetic; no device transports are instantiated.
        if subprocess.run(command, cwd=ROOT, check=False).returncode:
            raise Rejected('Offline firmware checks failed')
    if app_path:
        app_path = Path(app_path).resolve()
        flutter = shutil.which(flutter or 'flutter')
        if not flutter or not (app_path / 'pubspec.yaml').is_file():
            raise Rejected('Provide Flutter and the companion flutter_app path')
        if subprocess.run([sys.executable, '-m', 'unittest', 'discover', '-s', 'tool/tests'], cwd=app_path, check=False).returncode:
            raise Rejected('Offline App tool checks failed')
        flutter = str(Path(flutter).resolve())
        dart = str(Path(flutter).with_name('dart'))
        for command in ([flutter, 'test', '--no-pub'], [flutter, 'analyze', '--no-pub'],
                        [dart, 'run', 'tool/check_ble_contract.dart']):
            if subprocess.run(command, cwd=app_path, check=False).returncode:
                raise Rejected('Offline App checks failed')
    return {'offline_checks_passed': True, 'app_checked': bool(app_path), 'device_actions': False}
