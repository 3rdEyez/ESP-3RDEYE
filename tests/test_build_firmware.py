"""Exercise the public Python build entry point without a hardware toolchain."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "tools" / "build_firmware.py"


class BuildFirmwareEntryTest(unittest.TestCase):
    def setUp(self) -> None:
        prefix = "firmware-build-" if os.name == "nt" else "firmware build "
        self.temp = tempfile.TemporaryDirectory(prefix=prefix)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "project"
        (self.root / "tools").mkdir(parents=True)
        # On Windows, resolve expands any short (8.3) path aliases just as
        # the entry point does when locating its own project directory.
        self.root = self.root.resolve()
        shutil.copyfile(SOURCE, self.root / "tools" / "build_firmware.py")
        for name in ("sdkconfig.defaults", "sdkconfig.ble_primary.defaults",
                     "sdkconfig.legacy_udp.defaults"):
            (self.root / name).write_text("", encoding="utf-8")
        (self.root / "sdkconfig").write_text("existing private config", encoding="utf-8")

        self.idf = Path(self.temp.name) / "esp-idf" / "tools"
        self.idf.mkdir(parents=True)
        (self.idf / "idf.py").write_text(
            "import json, os, sys\n"
            "from pathlib import Path\n"
            "if sys.argv[1:] == ['--version']:\n"
            "    print(os.environ.get('SATORI_TEST_VERSION', 'ESP-IDF v5.5.4'))\n"
            "else:\n"
            "    Path(os.environ['SATORI_TEST_CAPTURE']).write_text(json.dumps(sys.argv[1:]))\n",
            encoding="utf-8",
        )
        self.capture = Path(self.temp.name) / "arguments.json"
        self.env = os.environ.copy()
        self.env["IDF_PATH"] = str(self.idf.parent)
        self.env["SATORI_TEST_CAPTURE"] = str(self.capture)
        self.env.pop("IDF_PYTHON_ENV_PATH", None)

    def invoke(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(self.root / "tools" / "build_firmware.py"), *args],
            cwd=self.temp.name, env=self.env, text=True, capture_output=True,
            check=False,
        )

    def test_default_and_legacy_profiles_keep_config_separate(self) -> None:
        for profile, args in (("ble_primary", ()), ("legacy_udp", ("legacy_udp",))):
            with self.subTest(profile=profile):
                result = self.invoke(*args)
                self.assertEqual(result.returncode, 0, result.stderr)
                passed = json.loads(self.capture.read_text(encoding="utf-8"))
                self.assertEqual(passed[0:2], ["-B", (self.root / "build" / profile).as_posix()])
                self.assertEqual(passed[2:4], ["-D", "SDKCONFIG=" +
                                 (self.root / "build" / profile / "sdkconfig").as_posix()])
                self.assertEqual(passed[4:6], ["-D", "SDKCONFIG_DEFAULTS=" +
                                 ";".join((self.root / name).as_posix() for name in
                                          ("sdkconfig.defaults", f"sdkconfig.{profile}.defaults"))])
                self.assertEqual(passed[6], "build")
                self.assertEqual((self.root / "sdkconfig").read_text(encoding="utf-8"),
                                 "existing private config")

    def test_wrong_idf_version_never_starts_build(self) -> None:
        self.env["SATORI_TEST_VERSION"] = "ESP-IDF v5.5.3"
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("Expected ESP-IDF v5.5.4", result.stderr)
        self.assertFalse(self.capture.exists())


if __name__ == "__main__":
    unittest.main()
