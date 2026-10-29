"""Tests for the GDB wrapper Python version check."""

import pathlib
import tempfile
import unittest

from bazel.toolchains.cc.mongo_linux.gdb_python_version_check import (
    check_gdb_wrapper_python_version,
)


class GdbPythonVersionCheckTest(unittest.TestCase):
    def test_matching_version(self):
        wrapper = 'python3_version = "3.13"\npythonhome = "/stow/python313-v5"\n'
        with tempfile.TemporaryDirectory() as temp_dir:
            wrapper_path = pathlib.Path(temp_dir) / "mongo_gdb.bzl"
            wrapper_path.write_text(wrapper, encoding="utf-8")

            self.assertEqual(
                check_gdb_wrapper_python_version(wrapper_path, expected_version="3.13"), []
            )

    def test_mismatching_version(self):
        wrapper = 'python3_version = "3.10"\npythonhome = "/stow/python310-v5"\n'
        with tempfile.TemporaryDirectory() as temp_dir:
            wrapper_path = pathlib.Path(temp_dir) / "mongo_gdb.bzl"
            wrapper_path.write_text(wrapper, encoding="utf-8")

            errors = check_gdb_wrapper_python_version(wrapper_path, expected_version="3.13")

            self.assertEqual(len(errors), 2)
            self.assertIn("uses Python 3.10", errors[0])
            self.assertIn("python313-", errors[1])


if __name__ == "__main__":
    unittest.main()
