import unittest

from buildscripts.resmokelib.core.programs import _format_shell_vars


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
