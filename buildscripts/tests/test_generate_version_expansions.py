"""Unit tests for buildscripts/generate_version_expansions.py."""

import os
import tempfile
import unittest
from unittest.mock import patch

from buildscripts import generate_version_expansions as under_test


class GenerateVersionExpansionsTest(unittest.TestCase):
    def generate_version_expansions(self, env: dict) -> dict:
        with tempfile.TemporaryDirectory() as temp_dir:
            original_cwd = os.getcwd()
            try:
                os.chdir(temp_dir)
                with patch.dict(
                    os.environ,
                    env,
                    clear=True,
                ):
                    expansions = under_test.generate_version_expansions()
            finally:
                os.chdir(original_cwd)

        return expansions

    def test_ga_version_string_is_not_release(self):
        env = {
            "MONGO_VERSION": "9.0.0",
        }

        expansions = self.generate_version_expansions(env)

        self.assertEqual("false", expansions["is_release"])
        self.assertEqual("latest", expansions["suffix"])
        self.assertEqual("latest", expansions["src_suffix"])

    def test_ga_version_string_with_override_is_release(self):
        env = {
            "MONGO_VERSION": "9.0.0",
            "IS_RELEASE": "true",
        }

        expansions = self.generate_version_expansions(env)

        self.assertEqual("true", expansions["is_release"])
        self.assertEqual("9.0.0", expansions["suffix"])
        self.assertEqual("r9.0.0", expansions["src_suffix"])

    def test_patch_alpha_version_defaults_to_not_release(self):
        env = {"MONGO_VERSION": "r9.0.0-alpha0-patch-version_id"}

        expansions = self.generate_version_expansions(env)

        self.assertEqual("false", expansions["is_release"])
        self.assertEqual("latest", expansions["suffix"])
        self.assertEqual("latest", expansions["src_suffix"])

    def test_explicit_is_release_override_survives_patch_alpha_version(self):
        env = {
            "MONGO_VERSION": "r9.0.0-alpha0-patch-version_id",
            "IS_RELEASE": "true",
        }

        expansions = self.generate_version_expansions(env)
        self.assertEqual("true", expansions["is_release"])
        self.assertEqual("9.0.0-alpha0-patch-version_id", expansions["suffix"])
        self.assertEqual("r9.0.0-alpha0-patch-version_id", expansions["src_suffix"])


if __name__ == "__main__":
    unittest.main()
