"""Tests for optional compression in artifact archiving."""

import sys
import tarfile
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import patch

from buildscripts.archive_artifacts import create_tarball


class TestArchiveArtifacts(unittest.TestCase):
    def test_gzip_fallback_without_zstd_implementation(self):
        # A marker-excluded Bazel dependency can still expose an empty namespace.
        for module in (None, types.ModuleType("pyzstd")):
            with self.subTest(module=module), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source = root / "support.txt"
                source.write_text("support files")
                output = root / "artifacts.tgz"
                with (
                    patch.dict(sys.modules, {"pyzstd": module}),
                    patch(
                        "buildscripts.archive_artifacts.shutil.which",
                        side_effect=lambda name: "/usr/bin/zstd" if name == "zstd" else None,
                    ),
                ):
                    create_tarball(str(output), [str(source)], [])
                with tarfile.open(output, "r:gz") as archive:
                    self.assertEqual(len(archive.getmembers()), 1)
                    member = archive.extractfile(archive.getmembers()[0])
                    assert member is not None
                    with member:
                        self.assertEqual(member.read(), b"support files")
                self.assertFalse(Path(str(output) + ".zst").exists())


if __name__ == "__main__":
    unittest.main()
