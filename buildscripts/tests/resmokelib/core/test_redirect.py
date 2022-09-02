"""Unit tests for buildscripts/resmokelib/core/redirect.py."""

from __future__ import absolute_import

import io
import os
import subprocess
import tempfile
import unittest

from buildscripts.resmokelib.core import redirect as _redirect


class TestStdoutRedirect(unittest.TestCase):
    is_windows = os.name == "nt"

    def test_process_pipes(self):
        """Write a string, one word per line into the beginning of a chain of processes. The input
        will be tee'd into a temporary file and grepped. Verify the contents of the tee'd file and
        the final output of the grep.
        """

        if self.is_windows:
            return

        haystack = "can you find a needle in a haystack".split()
        tmp_file = tempfile.mktemp()

        try:
            tee = _redirect.Pipe(["tee", tmp_file], subprocess.PIPE, subprocess.PIPE)
            grep = _redirect.Pipe(["grep", "needle"], tee.get_stdout(), subprocess.PIPE)

            for word in haystack:
                # Write the message with a newline after each word. Grep should only match "needle".
                tee.proc.stdin.write((word + "\n").encode("utf-8"))

            tee.get_stdin().close()
            self.assertEqual(b"needle", grep.get_stdout().read().strip())

            with open(tmp_file) as teed_file:
                self.assertEqual("\n".join(haystack) + "\n", teed_file.read())
        finally:
            tee.wait()
            grep.wait()
            tee.get_stdout().close()
            grep.get_stdout().close()
            os.remove(tmp_file)

    def test_stdout_rewrite(self):
        string = "mary had a little lamb"
        acc = io.BytesIO()
        with _redirect.StdoutRewrite(acc):
            print(string)

        print("not to be captured")
        self.assertEqual(string + "\n", acc.getvalue().decode("utf-8"))
