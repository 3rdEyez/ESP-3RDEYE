import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from satori_tools.runtime import Rejected, sdk_command
from satori_tools.usb_transport import require_rom_tool


class SdkCompatibility(unittest.TestCase):
    def test_exact_supported_rom_versions_and_unknown_fail_closed(self):
        for version in ('4.8.1', '4.12.0'):
            require_rom_tool(version)
        for version in ('4.12.1', '4.8.0', '5.0.0', ''):
            with self.assertRaises(Rejected):
                require_rom_tool(version)

    def test_activated_idf_pip_sdk_layout_uses_same_python(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            env = {'IDF_PATH': str(root / 'idf'), 'IDF_PYTHON_ENV_PATH': str(root / 'python-env')}
            with patch.dict(os.environ, env, clear=True):
                command = sdk_command('espsecure')
                self.assertEqual(command[1:], ['-m', 'espsecure'])
                self.assertIn('python-env', command[0])
                with self.assertRaises(Rejected):
                    sdk_command('idf')

    def test_legacy_vendored_sdk_layout_remains_preferred(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / 'components/esptool_py/esptool/espsecure.py'
            script.parent.mkdir(parents=True)
            script.touch()
            with patch.dict(os.environ, {'IDF_PATH': str(root)}, clear=True):
                self.assertEqual(sdk_command('espsecure')[1], str(script))
