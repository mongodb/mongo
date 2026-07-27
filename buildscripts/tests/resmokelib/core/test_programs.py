import os
import stat
import sys
import tempfile
import unittest
from unittest import mock

from buildscripts.resmokelib.core.programs import (
    _format_shell_vars,
    get_binary_version_output,
)


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
