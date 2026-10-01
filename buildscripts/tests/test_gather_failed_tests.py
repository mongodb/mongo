"""Unit tests for buildscripts/gather_failed_tests.py."""

import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from buildscripts import gather_failed_tests as under_test


class TestGatherFailedTests(unittest.TestCase):
    def test_missing_build_events_file_skips_failed_test_gathering(self):
        original_cwd = os.getcwd()
        with tempfile.TemporaryDirectory() as tmpdir:
            try:
                os.chdir(tmpdir)
                with patch.dict(os.environ, {"BUILD_WORKSPACE_DIRECTORY": tmpdir}):
                    with patch("builtins.print") as mock_print:
                        under_test.main("missing_build_events.json")
            finally:
                os.chdir(original_cwd)

            self.assertFalse(Path(tmpdir, "dist-tests").exists())
            mock_print.assert_called_once()
            self.assertIn("missing_build_events.json", mock_print.call_args.args[0])


class TestCopyBinsToUpload(unittest.TestCase):
    def _run_in(self, tmpdir):
        upload_bin_dir = Path(tmpdir, "upload", "bin")
        upload_lib_dir = Path(tmpdir, "upload", "lib")
        upload_bin_dir.mkdir(parents=True)
        upload_lib_dir.mkdir(parents=True)
        original_cwd = os.getcwd()
        try:
            os.chdir(tmpdir)
            under_test._copy_bins_to_upload(upload_bin_dir, upload_lib_dir)
        finally:
            os.chdir(original_cwd)
        return upload_bin_dir, upload_lib_dir

    def test_collects_from_transitioned_config_without_bazel_bin_symlink(self):
        # A suite whose shard count comes from a rule transition is built in its own
        # configuration, so the relink leaves bazel-bin pointing elsewhere (or absent) and the
        # binaries are only reachable under bazel-out/<config>/bin/src.
        with tempfile.TemporaryDirectory() as tmpdir:
            src = Path(tmpdir, "bazel-out", "k8-opt-ST-abc123", "bin", "src", "mongo")
            src.mkdir(parents=True)
            (src / "mongod.debug").touch()
            (src / "libfoo.so").touch()

            upload_bin_dir, upload_lib_dir = self._run_in(tmpdir)

            self.assertTrue((upload_bin_dir / "mongod.debug").exists())
            self.assertTrue((upload_lib_dir / "libfoo.so").exists())

    def test_does_not_copy_twice_when_symlink_and_glob_overlap(self):
        # bazel-bin normally resolves into one of the globbed directories; the dedup by resolved
        # path keeps that from being walked twice.
        with tempfile.TemporaryDirectory() as tmpdir:
            src = Path(tmpdir, "bazel-out", "k8-opt", "bin", "src", "mongo")
            src.mkdir(parents=True)
            (src / "mongod.debug").touch()
            Path(tmpdir, "bazel-bin").symlink_to(Path(tmpdir, "bazel-out", "k8-opt", "bin"))

            upload_bin_dir, _ = self._run_in(tmpdir)

            self.assertEqual([p.name for p in upload_bin_dir.iterdir()], ["mongod.debug"])

    def test_no_bin_directories_is_not_an_error(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            upload_bin_dir, upload_lib_dir = self._run_in(tmpdir)
            self.assertEqual(list(upload_bin_dir.iterdir()), [])
            self.assertEqual(list(upload_lib_dir.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
