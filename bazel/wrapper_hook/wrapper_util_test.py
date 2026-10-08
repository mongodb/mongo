"""Tests for terminal streams used by the Bazel wrapper."""

import contextlib
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from bazel.wrapper_hook import wrapper_util


class TerminalStreamTest(unittest.TestCase):
    def setUp(self):
        for name in ("_EXISTING_TERMINAL_STDOUT", "_EXISTING_TERMINAL_STDERR"):
            patcher = mock.patch.object(wrapper_util, name, None)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.addCleanup(self.close_cached_streams)

    def close_cached_streams(self):
        for stream in (
            wrapper_util._EXISTING_TERMINAL_STDOUT,
            wrapper_util._EXISTING_TERMINAL_STDERR,
        ):
            if stream is not None:
                with contextlib.suppress(OSError):
                    stream.close()

    def test_stdout_and_stderr_use_their_own_cached_streams(self):
        with (
            tempfile.TemporaryFile(mode="w+") as stdout,
            tempfile.TemporaryFile(mode="w+") as stderr,
        ):
            with mock.patch.dict(
                os.environ,
                {
                    "MONGO_WRAPPER_STDOUT_FD": str(stdout.fileno()),
                    "MONGO_WRAPPER_STDERR_FD": str(stderr.fileno()),
                },
            ):
                terminal_stderr = wrapper_util.get_terminal_stream("MONGO_WRAPPER_STDERR_FD")
                terminal_stdout = wrapper_util.get_terminal_stream("MONGO_WRAPPER_STDOUT_FD")
                self.assertIsNot(terminal_stdout, terminal_stderr)
                self.assertIs(
                    terminal_stdout, wrapper_util.get_terminal_stream("MONGO_WRAPPER_STDOUT_FD")
                )
                self.assertIs(
                    terminal_stderr, wrapper_util.get_terminal_stream("MONGO_WRAPPER_STDERR_FD")
                )
                terminal_stdout.write("stdout\n")
                terminal_stderr.write("stderr\n")
                stdout.seek(0)
                stderr.seek(0)
                self.assertEqual(stdout.read(), "stdout\n")
                self.assertEqual(stderr.read(), "stderr\n")

    @unittest.skipIf(os.name == "nt", "Requires Unix file descriptors")
    def test_closing_inherited_descriptor_does_not_break_stream_or_shutdown(self):
        script = """
import os
from bazel.wrapper_hook.wrapper_util import get_terminal_stream

stream = get_terminal_stream("MONGO_WRAPPER_STDERR_FD")
os.close(int(os.environ["MONGO_WRAPPER_STDERR_FD"]))
stream.write("terminal output after the original descriptor closes\\n")
stream.flush()
"""
        with tempfile.TemporaryFile(mode="w+") as terminal:
            descriptor = terminal.fileno()
            result = subprocess.run(
                [sys.executable, "-c", script],
                env={**os.environ, "MONGO_WRAPPER_STDERR_FD": str(descriptor)},
                pass_fds=(descriptor,),
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, "")
            terminal.seek(0)
            self.assertEqual(
                terminal.read(), "terminal output after the original descriptor closes\n"
            )


if __name__ == "__main__":
    unittest.main()
