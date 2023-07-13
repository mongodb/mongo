"""Library functions for powercycle."""
import logging
import os

import getpass
import shlex
import stat
import subprocess
import sys

import yaml

from buildscripts.resmokelib.plugin import Subcommand
from buildscripts.resmokelib.powercycle import powercycle_constants
from buildscripts.resmokelib.powercycle.lib.remote_operations import RemoteOperations
from buildscripts.resmokelib.powercycle.lib.named_temp_file import NamedTempFile

LOGGER = logging.getLogger(__name__)


# pylint: disable=abstract-method
class PowercycleCommand(Subcommand):
    """Base class for remote operations to set up powercycle."""

    def __init__(self):
        """Initialize PowercycleCommand."""
        self.expansions = yaml.safe_load(open(powercycle_constants.EXPANSIONS_FILE))
        self.ssh_connection_options = f"-i powercycle.pem {powercycle_constants.DEFAULT_SSH_CONNECTION_OPTIONS}"
        self.sudo = "" if self.is_windows() else "sudo"
        # The username on the Windows image that powercycle uses is currently the default user.
        self.user = "Administrator" if self.is_windows() else getpass.getuser()
        self.user_host = self.user + "@" + self.expansions["private_ip_address"]

        self.remote_op = RemoteOperations(
            user_host=self.user_host,
            ssh_connection_options=self.ssh_connection_options,
        )

    @staticmethod
    def is_windows() -> bool:
        """:return: True if running on Windows."""
        return sys.platform == "win32" or sys.platform == "cygwin"

    @staticmethod
    def _call(cmd):
        cmd = shlex.split(cmd)
        # Use a common pipe for stdout & stderr for logging.
        process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        buff_stdout, _ = process.communicate()
        buff = buff_stdout.decode("utf-8", "replace")
        return process.poll(), buff


def execute_cmd(cmd, use_file=False):
    """Execute command and returns return_code, output from command."""

    orig_cmd = ""
    temp_file = None
    # Multi-commands need to be written to a temporary file to execute on Windows.
    # This is due to complications with invoking Bash in Windows.
    if use_file:
        orig_cmd = cmd
        temp_file = create_temp_executable_file(cmd)
        # The temporary file name will have '\' on Windows and needs to be converted to '/'.
        cmd = "bash -c {}".format(temp_file.replace("\\", "/"))

    # If 'cmd' is specified as a string, convert it to a list of strings.
    if isinstance(cmd, str):
        cmd = shlex.split(cmd)

    if use_file:
        LOGGER.debug("Executing '%s', tempfile contains: %s", cmd, orig_cmd)
    else:
        LOGGER.debug("Executing '%s'", cmd)

    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        output, _ = proc.communicate()
        output = output.decode("utf-8", "replace")
        error_code = proc.returncode
        if error_code:
            output = "Error executing cmd {}: {}".format(cmd, output)
    finally:
        if use_file and temp_file is not None:
            os.remove(temp_file)

    return error_code, output


def create_temp_executable_file(cmds):
    """Create an executable temporary file containing 'cmds'. Returns file name."""
    temp_file_name = NamedTempFile.create(newline="\n", suffix=".sh", directory="tmp")
    with NamedTempFile.get(temp_file_name) as temp_file:
        temp_file.write(cmds)
    os_st = os.stat(temp_file_name)
    os.chmod(temp_file_name, os_st.st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return temp_file_name
