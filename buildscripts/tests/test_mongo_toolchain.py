"""Unit tests for mongo_toolchain.py."""

import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.append("buildscripts")
import mongo_toolchain

# The toolchain repo is created by a module extension, so it is materialized
# under its canonical (mangled) name rather than "mongo_toolchain_v5".
CANONICAL_REPO_NAME = "_main~setup_mongo_toolchains~mongo_toolchain_v5"


class TestBazelToolchain(unittest.TestCase):
    @staticmethod
    def _repo_dir(output_base: Path) -> Path:
        return output_base / "external" / CANONICAL_REPO_NAME

    @classmethod
    def _create_toolchain(cls, output_base: Path) -> Path:
        repo = cls._repo_dir(output_base)
        (repo / "BUILD.bazel").parent.mkdir(parents=True, exist_ok=True)
        (repo / "BUILD.bazel").touch()
        toolchain = repo / "v5"
        for directory in ("bin", "include", "lib"):
            (toolchain / directory).mkdir(parents=True, exist_ok=True)
        return toolchain

    @classmethod
    def _query_location(cls, output_base: Path) -> str:
        """The `bazel query --output=location` line for the repo's BUILD file."""
        return (
            f"{cls._repo_dir(output_base) / 'BUILD.bazel'}:1:10: "
            "filegroup rule @mongo_toolchain_v5//:clang_tidy"
        )

    def test_uses_canonical_repository_directory(self):
        with tempfile.TemporaryDirectory() as tempdir:
            output_base = Path(tempdir)
            expected_path = self._create_toolchain(output_base)

            with patch.object(
                mongo_toolchain, "_execute_bazel", return_value=self._query_location(output_base)
            ):
                toolchain = mongo_toolchain._get_bazel_toolchain("v5")

            self.assertEqual(Path(toolchain.get_root_dir()), expected_path.resolve())

    def test_fetches_repository_when_missing(self):
        with tempfile.TemporaryDirectory() as tempdir:
            output_base = Path(tempdir)
            # The query still resolves, but nothing has been materialized yet.
            self._repo_dir(output_base).mkdir(parents=True)

            with (
                patch.object(
                    mongo_toolchain,
                    "_execute_bazel",
                    return_value=self._query_location(output_base),
                ),
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
                (self._repo_dir(output_base) / "v5").resolve(),
            )


class TestParseQueryLocation(unittest.TestCase):
    def test_posix_path(self):
        self.assertEqual(
            mongo_toolchain._parse_query_location(
                "/home/user/.cache/bazel/external/repo/BUILD.bazel:1:10: "
                "filegroup rule @mongo_toolchain_v5//:clang_tidy"
            ),
            "/home/user/.cache/bazel/external/repo/BUILD.bazel",
        )

    def test_windows_path_keeps_drive_letter(self):
        # The drive letter's colon must not be mistaken for the line separator.
        self.assertEqual(
            mongo_toolchain._parse_query_location(
                "C:\\data\\mci\\0a03\\external\\repo\\BUILD.bazel:1:10: "
                "filegroup rule @mongo_toolchain_v5//:clang_tidy"
            ),
            "C:\\data\\mci\\0a03\\external\\repo\\BUILD.bazel",
        )

    def test_unparseable_output_raises(self):
        with self.assertRaises(mongo_toolchain.MongoToolchainNotFoundError):
            mongo_toolchain._parse_query_location("no location here")


if __name__ == "__main__":
    unittest.main()
