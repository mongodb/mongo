"""Unit tests for mongo_toolchain.py."""

import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.append("buildscripts")
import mongo_toolchain


class TestBazelToolchain(unittest.TestCase):
    @staticmethod
    def _create_toolchain(root: Path) -> Path:
        toolchain = root / "external" / "mongo_toolchain_v5" / "v5"
        for directory in ("bin", "include", "lib"):
            (toolchain / directory).mkdir(parents=True, exist_ok=True)
        return toolchain

    def test_uses_repository_under_bazel_output_base(self):
        with tempfile.TemporaryDirectory() as tempdir:
            output_base = Path(tempdir)
            expected_path = self._create_toolchain(output_base)

            with patch.object(mongo_toolchain, "_get_bazel_output_base", return_value=output_base):
                toolchain = mongo_toolchain._get_bazel_toolchain("v5")

            self.assertEqual(Path(toolchain.get_root_dir()), expected_path.resolve())

    def test_fetches_repository_under_bazel_output_base(self):
        with tempfile.TemporaryDirectory() as tempdir:
            output_base = Path(tempdir)

            with (
                patch.object(mongo_toolchain, "_get_bazel_output_base", return_value=output_base),
                patch.object(
                    mongo_toolchain,
                    "_fetch_bazel_toolchain",
                    side_effect=lambda version: self._create_toolchain(output_base),
                ) as fetch_toolchain,
            ):
                toolchain = mongo_toolchain._get_bazel_toolchain("v5")

            fetch_toolchain.assert_called_once_with("v5")
            self.assertEqual(
                Path(toolchain.get_root_dir()),
                (output_base / "external" / "mongo_toolchain_v5" / "v5").resolve(),
            )


if __name__ == "__main__":
    unittest.main()
