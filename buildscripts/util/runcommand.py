"""Utility to support running a command in a subprocess."""

from __future__ import print_function

import os
import pipes
import shlex
import sys

# The subprocess32 module is untested on Windows and thus isn't recommended for use, even when it's
# installed. See https://github.com/google/python-subprocess32/blob/3.2.7/README.md#usage.
if os.name == "posix" and sys.version_info[0] == 2:
    try:
        import subprocess32 as subprocess
    except ImportError:
        import warnings
        warnings.warn(("Falling back to using the subprocess module because subprocess32 isn't"
                       " available. When using the subprocess module, a child process may"
                       " trigger an invalid free(). See SERVER-22219 for more details."),
                      RuntimeWarning)
        import subprocess  # type: ignore
else:
    import subprocess


def execute_cmd(cmd):
    """Execute specified 'cmd' and return err_code and output.

    If 'cmd' is specified as a string, convert it to a list of strings.
    """
    if isinstance(cmd, str):
        cmd = shlex.split(cmd)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output, _ = proc.communicate()
    error_code = proc.returncode
    return error_code, output


class RunCommand(object):
    """Class to abstract executing a subprocess."""

    def __init__(self, string=None):
        """Initialize the RunCommand object."""
        self._command = string

    def add(self, string):
        """Add a string to the command."""
        self._command = "{}{}{}".format(self._command, self._space(), string)

    def add_file(self, path):
        """Add a file path to the command."""
        # For Windows compatability, use pipes.quote around file paths.
        self._command = "{}{}{}".format(self._command, self._space(), pipes.quote(path))

    def execute(self):
        """Execute the command. Return (error_code, output)."""
        return execute_cmd(self._command)

    @property
    def command(self):
        """Get the command."""
        return self._command

    def _space(self):
        """Return a space if the command has been started to be built."""
        if self._command:
            return " "
        return ""
