"""Utility to support running a command in a subprocess."""

import os
import pipes
import shlex
import sys
import subprocess

from . import fileops


class RunCommand(object):
    """Class to abstract executing a subprocess."""

    def __init__(self, string=None, output_file=None, append_file=False, propagate_signals=True):
        """Initialize the RunCommand object."""
        self._command = string if string else ""
        self.output_file = output_file
        self.append_file = append_file
        self._process = None
        if propagate_signals or os.name != "posix":
            # The function os.setpgrp is not supported on Windows.
            self._preexec_kargs = {}
        elif subprocess.__name__ == "subprocess32":
            self._preexec_kargs = {"start_new_session": True}
        else:
            self._preexec_kargs = {"preexec_fn": os.setpgrp}

    def add(self, string):
        """Add a string to the command."""
        self._command = "{}{}{}".format(self._command, self._space(), string)

    def add_file(self, path):
        """Add a file path to the command."""
        # For Windows compatability, use pipes.quote around file paths.
        self._command = "{}{}{}".format(self._command, self._space(), pipes.quote(path))

    def _space(self):
        """Return a space if the command has been started to be built."""
        if self._command:
            return " "
        return ""

    def _cmd_list(self):
        """Return 'cmd' as a list of strings."""
        cmd = self._command
        if isinstance(cmd, str):
            cmd = shlex.split(cmd)
        return cmd

    def execute(self):
        """Execute 'cmd' and return err_code and output."""
        self._process = subprocess.Popen(self._cmd_list(), stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT, **self._preexec_kargs)
        output, _ = self._process.communicate()
        error_code = self._process.returncode
        return error_code, output

    def execute_with_output(self):
        """Execute the command, return result as a string."""
        return subprocess.check_output(self._cmd_list()).decode('utf-8')

    def execute_save_output(self):
        """Execute the command, save result in 'self.output_file' and return returncode."""
        with fileops.get_file_handle(self.output_file, self.append_file) as file_handle:
            ret = subprocess.check_call(self._cmd_list(), stdout=file_handle)
        return ret

    def start_process(self):
        """Start to execute the command."""
        # Do not propagate interrupts to the child process.
        with fileops.get_file_handle(self.output_file, self.append_file) as file_handle:
            self._process = subprocess.Popen(self._cmd_list(), stdin=subprocess.PIPE,
                                             stdout=file_handle, stderr=subprocess.STDOUT,
                                             **self._preexec_kargs)

    def send_to_process(self, string=None):
        """Send 'string' to a running processs and return stdout, stderr."""
        return self._process.communicate(string)

    def wait_for_process(self):
        """Wait for a running processs to end and return stdout, stderr."""
        return self.send_to_process()

    def stop_process(self):
        """Stop the running process."""
        self._process.terminate()

    def kill_process(self):
        """Kill the running process."""
        self._process.kill()

    def is_process_running(self):
        """Return True if the process is still running."""
        return self._process.poll() is None

    @property
    def command(self):
        """Get the command."""
        return self._command

    @property
    def process(self):
        """Get the process object."""
        return self._process
