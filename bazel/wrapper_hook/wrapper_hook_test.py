"""Regression tests for wrapper output when CI has no terminal descriptors."""

import io
import unittest
from unittest.mock import patch

from bazel.wrapper_hook import wrapper_hook


class WrapperOutputTest(unittest.TestCase):
    def test_info_is_preserved_without_terminal_descriptor(self):
        output = io.StringIO()
        with (
            patch.object(wrapper_hook, "get_terminal_stream", return_value=None),
            patch.object(wrapper_hook.sys, "stdout", output),
            patch.object(wrapper_hook.sys, "stderr", output),
        ):
            wrapper_hook._info("lint diagnostic")
        self.assertEqual(output.getvalue(), "INFO: lint diagnostic\n")

    def test_output_is_preserved_without_terminal_descriptor(self):
        output = io.StringIO()
        with (
            patch.object(wrapper_hook, "get_terminal_stream", return_value=None),
            patch.object(wrapper_hook.sys, "stdout", output),
            patch.object(wrapper_hook.sys, "stderr", output),
        ):
            wrapper_hook.run_with_terminal_output(lambda: print("lint diagnostic"))
            self.assertIs(wrapper_hook.sys.stdout, output)
            self.assertIs(wrapper_hook.sys.stderr, output)
        self.assertEqual(output.getvalue(), "lint diagnostic\n")

    def test_streams_are_restored_after_exception(self):
        output = io.StringIO()
        terminal = io.StringIO()
        with (
            patch.object(wrapper_hook, "get_terminal_stream", return_value=terminal),
            patch.object(wrapper_hook.sys, "stdout", output),
            patch.object(wrapper_hook.sys, "stderr", output),
        ):

            def fail():
                print("lint diagnostic")
                raise ValueError("failure")

            with self.assertRaises(ValueError):
                wrapper_hook.run_with_terminal_output(fail)
            self.assertIs(wrapper_hook.sys.stdout, output)
            self.assertIs(wrapper_hook.sys.stderr, output)
        self.assertEqual(terminal.getvalue(), "lint diagnostic\n")


if __name__ == "__main__":
    unittest.main()
