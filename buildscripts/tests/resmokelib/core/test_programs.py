import logging
import os
import stat
import sys
import tempfile
import unittest
from unittest import mock

from buildscripts.resmokelib.core.programs import (
    _format_shell_vars,
    get_binary_version,
    get_binary_version_output,
    get_version_suffix,
    mongod_program,
)
from buildscripts.resmokelib.utils.history import make_historic


class GetBinaryVersionOutputTestCase(unittest.TestCase):
    def test_resolves_bare_binary_via_augmented_path(self):
        """A bare binary name living in a multiversion dir must be resolved to an absolute
        path before invocation.
        """
        get_binary_version_output.cache_clear()

        exe_suffix = ".exe" if sys.platform == "win32" else ""

        # Create a multiversion binary in a separate directory and look it up by its bare name.
        with tempfile.TemporaryDirectory() as multiversion_dir:
            binary_name = "mongod-99.0"
            binary_path = os.path.join(multiversion_dir, binary_name + exe_suffix)
            with open(binary_path, "w") as f:
                f.write("")
            os.chmod(binary_path, os.stat(binary_path).st_mode | stat.S_IEXEC)

            with (
                mock.patch(
                    "buildscripts.resmokelib.core.programs.config.MULTIVERSION_DIRS",
                    [multiversion_dir],
                ),
                mock.patch(
                    "buildscripts.resmokelib.core.programs.check_output",
                    return_value=b"db version v99.0.0",
                ) as mock_check_output,
            ):
                output = get_binary_version_output(binary_name)

            self.assertEqual(output, "db version v99.0.0")
            called_args = mock_check_output.call_args[0][0]
            # The bare name must have been resolved to the absolute path in the multiversion dir.
            self.assertEqual(len(called_args), 2)
            self.assertEqual(os.path.normcase(called_args[0]), os.path.normcase(binary_path))
            self.assertEqual(called_args[1], "--version")


class GetVersionSuffixTestCase(unittest.TestCase):
    def test_suffixed_binary(self):
        self.assertEqual(get_version_suffix("mongod-9.0"), "9.0")
        self.assertEqual(get_version_suffix("/data/multiversion/mongod-8.2"), "8.2")

    def test_latest_binary(self):
        self.assertIsNone(get_version_suffix("mongod"))
        self.assertIsNone(get_version_suffix("/data/mci/abc/src/dist-test/bin/mongod"))


class GetBinaryVersionTestCase(unittest.TestCase):
    def test_suffixed_binary(self):
        self.assertEqual(get_binary_version("mongod-8.2"), "8.2")

    def test_latest_binary(self):
        from buildscripts.resmokelib.multiversionconstants import LATEST_FCV

        self.assertEqual(get_binary_version("mongod"), LATEST_FCV)


class MongodProgramSetParametersTestCase(unittest.TestCase):
    PARAM = "migrationRecipientPITHistoryToPreserveInSecs"

    def _final_set_parameters(self, executable):
        mongod_options = make_historic({"set_parameters": {self.PARAM: 1}, "port": 12345})
        _, final_options = mongod_program(
            logging.getLogger(__name__), 0, executable, {}, mongod_options
        )
        return final_options["set_parameters"]

    def test_strips_the_parameter_only_for_multiversion_binaries_that_predate_it(self):
        # The 9.0 branch reports the same version as master (9.0), so the suffix is what
        # identifies a downloaded binary that does not have the parameter.
        self.assertNotIn(self.PARAM, self._final_set_parameters("mongod-9.0"))
        self.assertNotIn(self.PARAM, self._final_set_parameters("/data/multiversion/mongod-8.2"))
        self.assertIn(self.PARAM, self._final_set_parameters("mongod-9.1"))
        self.assertIn(self.PARAM, self._final_set_parameters("mongod"))
        self.assertIn(
            self.PARAM, self._final_set_parameters("/data/mci/abc/src/dist-test/bin/mongod")
        )


class ResmokeProgramsTestCase(unittest.TestCase):
    def test_format_shell_vars_with_dot(self):
        string_builder = []
        with_dot = {"a.b": "c"}
        _format_shell_vars(string_builder, ["dummy_key"], with_dot)
        expected = ["dummy_key = new Object()", 'dummy_key["a.b"] = "c"']
        self.assertEqual(string_builder, expected)

        string_builder = []
        without_dot = {"a": {"b": "c"}}
        _format_shell_vars(string_builder, ["dummy_key"], without_dot)
        expected = [
            "dummy_key = new Object()",
            'dummy_key["a"] = new Object()',
            'dummy_key["a"]["b"] = "c"',
        ]
        self.assertEqual(string_builder, expected)


if __name__ == "__main__":
    unittest.main()
