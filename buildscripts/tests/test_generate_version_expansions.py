"""Unit tests for buildscripts/generate_version_expansions.py."""

import os
import tempfile
import unittest
from unittest.mock import patch

from buildscripts import generate_version_expansions as under_test


class GenerateVersionExpansionsTest(unittest.TestCase):
    def test_patch_alpha_version_defaults_to_not_release(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            original_cwd = os.getcwd()
            try:
                os.chdir(temp_dir)
                with patch.dict(
                    os.environ,
                    {"MONGO_VERSION": "r9.0.0-alpha0-patch-version_id"},
                    clear=True,
                ):
                    expansions = under_test.generate_version_expansions()
            finally:
                os.chdir(original_cwd)

        self.assertEqual("false", expansions["is_release"])

    def test_explicit_is_release_override_survives_patch_alpha_version(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            original_cwd = os.getcwd()
            try:
                os.chdir(temp_dir)
                with patch.dict(
                    os.environ,
                    {
                        "MONGO_VERSION": "r9.0.0-alpha0-patch-version_id",
                        "IS_RELEASE": "true",
                    },
                    clear=True,
                ):
                    expansions = under_test.generate_version_expansions()
            finally:
                os.chdir(original_cwd)

        self.assertEqual("true", expansions["is_release"])


if __name__ == "__main__":
    unittest.main()
