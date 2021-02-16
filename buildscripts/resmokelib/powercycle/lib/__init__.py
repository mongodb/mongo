"""Library functions for powercycle."""

import getpass
import shlex
import subprocess
import sys

import yaml

from buildscripts.resmokelib.plugin import Subcommand
from buildscripts.resmokelib.powercycle import powercycle_constants
from buildscripts.resmokelib.powercycle.lib.remote_operations import RemoteOperations


class PowercycleCommand(Subcommand):  # pylint: disable=abstract-method, too-many-instance-attributes
    """Base class for remote operations to set up powercycle."""

    def __init__(self):
        """Initialize PowercycleCommand."""

        self.expansions = yaml.safe_load(open(powercycle_constants.EXPANSIONS_FILE))

        self.retries = 0 if "ssh_retries" not in self.expansions else int(
            self.expansions["ssh_retries"])
        self.ssh_identity = self._get_ssh_identity()
        self.ssh_connection_options = self.ssh_identity + " " + self.expansions[
            "ssh_connection_options"]
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

    def _get_posix_workdir(self) -> str:
        workdir = self.expansions['workdir']
        if self.is_windows():
            workdir = workdir.replace("\\", "/")
        return workdir

    def _get_ssh_identity(self) -> str:
        workdir = self._get_posix_workdir()
        pem_file = '/'.join([workdir, 'powercycle.pem'])

        return f"-i {pem_file}"
