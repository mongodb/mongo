"""Unit tests for version_constants_gen.py module list generation."""

import json
import os
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src", "mongo", "util"))
import version_constants_gen


class TestModulesList(unittest.TestCase):
    """Test that the modules list is correctly generated based on build flags."""

    def _make_extra_definitions(self, enterprise=False, atlas=False):
        defs = {
            "MONGO_DISTMOD": "",
            "MONGO_VERSION": "9.0.0",
            "GIT_COMMIT_HASH": "abc123",
            "js_engine_ver": "mozjs",
            "MONGO_ALLOCATOR": "tcmalloc-google",
            "compile_variables": "",
            "linkflags": "",
            "cpp_defines": "",
        }
        if enterprise:
            defs["build_enterprise_enabled"] = "1"
        if atlas:
            defs["build_atlas_enabled"] = "1"
        return json.dumps(defs)

    @patch("version_constants_gen.get_toolchain_ver", return_value="mock-compiler: version unknown")
    def test_no_modules(self, _mock_toolchain):
        """Test that no modules are listed when neither enterprise nor atlas is enabled."""
        with tempfile.NamedTemporaryFile(mode="w", suffix=".log", delete=False) as logfile:
            logpath = logfile.name
        try:
            result = version_constants_gen.generate_config_header(
                compiler_path="gcc",
                compiler_args="",
                env_vars=json.dumps({}),
                logpath=logpath,
                additional_inputs="",
                extra_definitions=self._make_extra_definitions(),
            )
            self.assertEqual(result["@buildinfo_modules@"], "")
        finally:
            os.unlink(logpath)

    @patch("version_constants_gen.get_toolchain_ver", return_value="mock-compiler: version unknown")
    def test_enterprise_only(self, _mock_toolchain):
        """Test that only enterprise module is listed when only enterprise is enabled."""
        with tempfile.NamedTemporaryFile(mode="w", suffix=".log", delete=False) as logfile:
            logpath = logfile.name
        try:
            result = version_constants_gen.generate_config_header(
                compiler_path="gcc",
                compiler_args="",
                env_vars=json.dumps({}),
                logpath=logpath,
                additional_inputs="",
                extra_definitions=self._make_extra_definitions(enterprise=True),
            )
            self.assertEqual(result["@buildinfo_modules@"], '"enterprise"_sd')
        finally:
            os.unlink(logpath)

    @patch("version_constants_gen.get_toolchain_ver", return_value="mock-compiler: version unknown")
    def test_atlas_only(self, _mock_toolchain):
        """Test that only atlas module is listed when only atlas is enabled."""
        with tempfile.NamedTemporaryFile(mode="w", suffix=".log", delete=False) as logfile:
            logpath = logfile.name
        try:
            result = version_constants_gen.generate_config_header(
                compiler_path="gcc",
                compiler_args="",
                env_vars=json.dumps({}),
                logpath=logpath,
                additional_inputs="",
                extra_definitions=self._make_extra_definitions(atlas=True),
            )
            self.assertEqual(result["@buildinfo_modules@"], '"atlas"_sd')
        finally:
            os.unlink(logpath)

    @patch("version_constants_gen.get_toolchain_ver", return_value="mock-compiler: version unknown")
    def test_enterprise_and_atlas(self, _mock_toolchain):
        """Test that both modules are listed when both enterprise and atlas are enabled."""
        with tempfile.NamedTemporaryFile(mode="w", suffix=".log", delete=False) as logfile:
            logpath = logfile.name
        try:
            result = version_constants_gen.generate_config_header(
                compiler_path="gcc",
                compiler_args="",
                env_vars=json.dumps({}),
                logpath=logpath,
                additional_inputs="",
                extra_definitions=self._make_extra_definitions(enterprise=True, atlas=True),
            )
            self.assertEqual(result["@buildinfo_modules@"], '"enterprise"_sd,\n"atlas"_sd')
        finally:
            os.unlink(logpath)


if __name__ == "__main__":
    unittest.main()
