import importlib.util
import ast
import os
from pathlib import Path
import sys
import unittest
from types import SimpleNamespace
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import satori_dev
class Tests(unittest.TestCase):
    def test_new_default_and_historical_profiles_explicit(self):
        self.assertEqual(satori_dev.parser().parse_args(['build']).profile, 'ble_wifi_ota_prototype')
        for name in ('ble_primary', 'legacy_udp'):
            self.assertEqual(satori_dev.parser().parse_args(['build',name]).profile,name)
        defaults=(ROOT/'sdkconfig.defaults').read_text()
        self.assertIn('CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.dual_ota.csv"',defaults)
        self.assertIn('CONFIG_SATORI_WIFI_OTA_PROTOTYPE=y',defaults)
        self.assertIn('CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n',defaults)
        for name in ('ble_primary','legacy_udp'):
            text=(ROOT/('sdkconfig.'+name+'.defaults')).read_text()
            self.assertIn('CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"',text)
            self.assertIn('CONFIG_SATORI_WIFI_OTA_PROTOTYPE=n',text)
            self.assertIn('CONFIG_SATORI_DUAL_OTA_BOOT_CONFIRM=n',text)
    def test_direct_sdk_write_guard_has_no_device_imports(self):
        spec=importlib.util.spec_from_file_location('guard',ROOT/'idf_ext.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        module.guard([SimpleNamespace(name='build')])
        for name in module.BLOCKED:
            with self.assertRaisesRegex(RuntimeError,'migrate_layout'):module.guard([SimpleNamespace(name=name)])
        self.assertIn('satori_block_direct_flash',(ROOT/'CMakeLists.txt').read_text())

    @unittest.skipUnless(os.environ.get('IDF_PATH'), 'Pinned SDK action audit needs IDF_PATH')
    def test_pinned_sdk_registered_write_actions_blocked(self):
        spec=importlib.util.spec_from_file_location('guard',ROOT/'idf_ext.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        tree=ast.parse((Path(os.environ['IDF_PATH'])/'tools/idf_py_actions/serial_ext.py').read_text())
        names={key.value for node in ast.walk(tree) if isinstance(node,ast.Dict) for key in node.keys
               if isinstance(key,ast.Constant) and isinstance(key.value,str)}
        writes={name for name in names if name in ('flash','erase_flash','erase-flash','erase-otadata') or name.endswith('-flash') and not name.startswith('read-')}
        self.assertIn('erase_flash',writes)
        self.assertTrue(writes <= module.BLOCKED, writes-module.BLOCKED)
