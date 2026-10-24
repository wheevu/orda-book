"""Check standalone manifest validation and failure propagation."""

import copy
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ManifestValidationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "standalone"
        (self.root / "scripts").mkdir(parents=True)
        (self.root / "results").mkdir()
        for name in ["check_manifest.sh", "validate_manifest.py"]:
            shutil.copy2(ROOT / "scripts" / name, self.root / "scripts" / name)
        self.manifest = json.loads((ROOT / "results/manifest.json").read_text())

    def run_check(self, manifest):
        (self.root / "results/manifest.json").write_text(json.dumps(manifest))
        return subprocess.run(
            ["sh", str(self.root / "scripts/check_manifest.sh")],
            cwd=self.temp.name,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_standalone_from_another_directory(self):
        result = self.run_check(self.manifest)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("manifest OK:", result.stdout)

    def test_invalid_metadata_fails_the_shell_check(self):
        for field, bad_value in [
            (("schema",), "unknown/results-manifest/v1"),
            (("campaign_id",), "Invalid Campaign"),
            (("source", "commit"), "not-a-commit"),
            (("execution", "argv"), []),
            (("artifacts", 0, "sha256"), "not-a-hash"),
        ]:
            with self.subTest(field=field):
                manifest = copy.deepcopy(self.manifest)
                target = manifest
                for key in field[:-1]:
                    target = target[key]
                target[field[-1]] = bad_value
                result = self.run_check(manifest)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("evidence checks passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
