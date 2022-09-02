#!/usr/bin/env python3
"""Remote access utilities, via ssh & scp."""

import os
import posixpath
import re
import shlex
import subprocess
import sys
import time
import textwrap

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

_IS_WINDOWS = sys.platform == "win32" or sys.platform == "cygwin"

_SSH_CONNECTION_ERRORS = [
    "Connection refused",
    "Connection timed out during banner exchange",
    "Permission denied",
    "System is booting up.",
    "ssh_exchange_identification: read: Connection reset by peer",
]


class SSHOperation(object):
    """Class to determine which SSH operation to run."""

    COPY_TO = "copy_to"
    COPY_FROM = "copy_from"
    SHELL = "shell"


def posix_path(path):
    """Return posix path, used on Windows since scp requires posix style paths."""
    # If path is already quoted, we need to remove the quotes before calling
    path_quote = "\'" if path.startswith("\'") else ""
    path_quote = "\"" if path.startswith("\"") else path_quote
    if path_quote:
        path = path[1:-1]
    drive, new_path = os.path.splitdrive(path)
    if drive:
        new_path = posixpath.join("/cygdrive", drive.split(":")[0], *re.split("/|\\\\", new_path))
    return "{quote}{path}{quote}".format(quote=path_quote, path=new_path)


class RemoteOperations(object):
    """Class to support remote operations."""

    def __init__(self, user_host, ssh_connection_options=None, ssh_options=None, scp_options=None,
                 shell_binary="/bin/bash", use_shell=False, ignore_ret=False, access_retry_count=5):
        """Initialize RemoteOperations."""

        self.user_host = user_host
        self.ssh_connection_options = ssh_connection_options if ssh_connection_options else ""
        self.ssh_options = ssh_options if ssh_options else ""
        self.scp_options = scp_options if scp_options else ""
        self.retry_sleep = 10
        self.ignore_ret = ignore_ret
        self.shell_binary = shell_binary
        self.use_shell = use_shell
        self.access_retry_count = access_retry_count
        # Check if we can remotely access the host.
        self._access_code, self._access_buff = self._remote_access()

    def _call(self, cmd):
        print(f"Executing command in subprocess: {cmd}")
        # If use_shell is False we need to split the command up into a list.
        if not self.use_shell:
            cmd = shlex.split(cmd)
        # Use a common pipe for stdout & stderr for logging.
        process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   shell=self.use_shell)
        buff_stdout, _ = process.communicate()
        buff = buff_stdout.decode("utf-8", "replace")
        print("Result of command:")
        print(textwrap.indent(buff, "[result body] "))
        return process.poll(), buff

    def _call_retries(self, cmd, retry_count):
        attempt_num = 0
        while True:
            ret, buff = self._call(cmd)
            # Ignore any connection errors before sshd has fully initialized.
            if not ret and not any(ssh_error in buff for ssh_error in _SSH_CONNECTION_ERRORS):
                return ret, buff
            attempt_num += 1
            if attempt_num > retry_count:
                print("Exhausted all retry attempts.")
                break
            print("Remote attempt {} unsuccessful, retrying in {} seconds".format(
                attempt_num, self.retry_sleep))
            time.sleep(self.retry_sleep)
        return ret, buff

    def _remote_access(self):
        """Check if a remote session is possible."""
        cmd = "ssh {} {} {} date".format(self.ssh_connection_options, self.ssh_options,
                                         self.user_host)
        return self._call_retries(cmd, self.access_retry_count)

    def _perform_operation(self, cmd, retry, retry_count):
        if retry:
            return self._call_retries(cmd, retry_count)

        return self._call(cmd)

    def access_established(self):
        """Return True if initial access was established."""
        return not self._access_code

    def access_info(self):
        """Print the return code and output buffer from initial access attempt(s)."""
        return self._access_code, self._access_buff

    @staticmethod
    def ssh_error(message):
        """Return True if the error message is generated from the ssh client.

        This can help determine if an error is due to a remote operation failing or an ssh
        related issue, like a connection issue.
        """
        return message.startswith("ssh:")

    # pylint: disable=inconsistent-return-statements
    def operation(self, operation_type, operation_param, operation_dir=None, retry=False,
                  retry_count=5):
        """Execute Main entry for remote operations. Returns (code, output).

        'operation_type' supports remote shell and copy operations.
        'operation_param' can either be a list or string of commands or files.
        'operation_dir' is '.' if unspecified for 'copy_*'.
        """

        if not self.access_established():
            code, output = self.access_info()
            print(f"Exiting, unable to establish access. Code=${code}, output=${output}")
            return

        # File names with a space must be quoted, since we permit the
        # the file names to be either a string or a list.
        if operation_type.startswith("copy") and isinstance(operation_param, str):
            operation_param = shlex.split(operation_param, posix=not _IS_WINDOWS)

        if operation_type == "shell":
            if operation_dir is not None:
                operation_param = "cd {}; {}".format(operation_dir, operation_param)
            dollar = ""
            if re.search("\"|'", operation_param):
                # To ensure any quotes in operation_param are handled correctly when
                # invoking the operation_param, escape with \ and add $ in the front.
                # See https://stackoverflow.com/questions/8254120/
                #   how-to-escape-a-single-quote-in-single-quote-string-in-bash
                operation_param = "{}".format(operation_param.replace("'", r"\'"))
                operation_param = "{}".format(operation_param.replace("\"", r"\""))
                dollar = "$"
            cmd = "ssh {} {} {} {} -c \"{}'{}'\"".format(self.ssh_connection_options,
                                                         self.ssh_options, self.user_host,
                                                         self.shell_binary, dollar, operation_param)

        elif operation_type == "copy_to":
            cmd = "scp -r {} {} ".format(self.ssh_connection_options, self.scp_options)
            # To support spaces in the filename or directory, we quote them one at a time.
            for copy_file in operation_param:
                # Quote file on Posix.
                quote = "\"" if not _IS_WINDOWS else ""
                cmd += "{quote}{file}{quote} ".format(quote=quote, file=posix_path(copy_file))
            operation_dir = operation_dir if operation_dir else ""
            cmd += " {}:{}".format(self.user_host, posix_path(operation_dir))

        elif operation_type == "copy_from":
            operation_dir = operation_dir if operation_dir else "."
            if not os.path.isdir(operation_dir):
                raise ValueError("Local directory '{}' does not exist.".format(operation_dir))

            # We support multiple files being copied from the remote host
            # by invoking scp for each file specified.
            # Note - this is a method which scp does not support directly.
            for copy_file in operation_param:
                copy_file = posix_path(copy_file)
                cmd = "scp -r {} {} {}:".format(self.ssh_connection_options, self.scp_options,
                                                self.user_host)
                # Quote (on Posix), and escape the file if there are spaces.
                # Note - we do not support other non-ASCII characters in a file name.
                quote = "\"" if not _IS_WINDOWS else ""
                if " " in copy_file:
                    copy_file = re.escape("{quote}{file}{quote}".format(
                        quote=quote, file=copy_file))
                cmd += "{} {}".format(copy_file, posix_path(operation_dir))

        else:
            raise ValueError(f"Invalid operation '{operation_type}' specified.")

        print(f"Created {operation_type} operation")
        buff = ""

        ret, new_buff = self._perform_operation(cmd, retry, retry_count)
        buff += new_buff

        if ret != 0:
            if self.ignore_ret:
                print(f"Ignoring return code {ret}.")
                return ret, buff
            raise Exception(buff)

        return ret, buff

    def shell(self, operation_param, operation_dir=None):
        """Provide helper for remote shell operations."""
        return self.operation(operation_type="shell", operation_param=operation_param,
                              operation_dir=operation_dir)

    def copy_to(self, operation_param, operation_dir=None):
        """Provide helper for remote copy_to operations."""
        return self.operation(operation_type="copy_to", operation_param=operation_param,
                              operation_dir=operation_dir)

    def copy_from(self, operation_param, operation_dir=None):
        """Provide helper for remote copy_from operations."""
        return self.operation(operation_type="copy_from", operation_param=operation_param,
                              operation_dir=operation_dir)
