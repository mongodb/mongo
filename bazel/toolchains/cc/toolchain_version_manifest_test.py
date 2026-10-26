import json
import tempfile
import unittest
from pathlib import Path

from bazel.toolchains.cc.toolchain_version_manifest import make_manifest


class ToolchainVersionManifestTest(unittest.TestCase):
    def test_capture_preserves_banners_and_only_enforces_production(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            compiler = Path(temporary) / "compiler.json"
            linker = Path(temporary) / "linker.json"
            for version, production in (("22.1.1", False), ("19.1.7", True)):
                compiler.write_text(
                    json.dumps(
                        {"compiler": "clang", "compiler_version": version, "cc": "clang " + version}
                    )
                )
                linker.write_text(
                    json.dumps({"linker": "lld", "linker_version": version, "ld": "LLD " + version})
                )
                manifest = make_manifest(compiler, linker, "macOS", "arm64", "cross", production)
                self.assertEqual(version, manifest["compiler_version"])
                self.assertEqual("LLD " + version, manifest["ld"])
                self.assertEqual("arm64", manifest["target_arch"])
                self.assertEqual("cross", manifest["toolchain_mode"])
                self.assertEqual(production, manifest["production_release"])
                if not production:
                    with self.assertRaisesRegex(RuntimeError, "compiler_version"):
                        make_manifest(compiler, linker, "macOS", "arm64", "cross", True)
            linker.write_text(json.dumps({"linker": "lld", "linker_version": "22.1.1"}))
            with self.assertRaisesRegex(RuntimeError, "linker_version"):
                make_manifest(compiler, linker, "macOS", "arm64", "cross", True)


if __name__ == "__main__":
    unittest.main()
