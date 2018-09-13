#!/usr/bin/env python

"""Remote access utilities, via ssh & scp."""

from __future__ import print_function

import optparse
import os
import posixpath
import re
import shlex
import sys
import time

# The subprocess32 module is untested on Windows and thus isn't recommended for use, even when it's
# installed. See https://github.com/google/python-subprocess32/blob/3.2.7/README.md#usage.
if os.name == "posix" and sys.version_info[0] == 2:
    try:
        import subprocess32 as subprocess
    except ImportError:
        import warnings
        warnings.warn(("Falling back to using the subprocess module because subprocess32 isn't"
                       " available. When using the subprocess module, a child process may trigger"
                       " an invalid free(). See SERVER-22219 for more details."),
                      RuntimeWarning)
        import subprocess
else:
    import subprocess

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

_IS_WINDOWS = sys.platform == "win32" or sys.platform == "cygwin"

_OPERATIONS = ["shell", "copy_to", "copy_from"]

_SSH_CONNECTION_ERRORS = [
    "Connection refused",
    "Connection timed out during banner exchange",
    "Permission denied",
    "System is booting up.",
    "ssh_exchange_identification: read: Connection reset by peer",
]


def posix_path(path):
    """ Returns posix path, used on Windows since scp requires posix style paths. """
    # If path is already quoted, we need to remove the quotes before calling
    path_quote = "\'" if path.startswith("\'") else ""
    path_quote = "\"" if path.startswith("\"") else path_quote
    if path_quote:
        path = path[1:-1]
    drive, new_path = os.path.splitdrive(path)
    if drive:
        new_path = posixpath.join(
            "/cygdrive",
            drive.split(":")[0],
            *re.split("/|\\\\", new_path))
    return "{quote}{path}{quote}".format(quote=path_quote, path=new_path)


class RemoteOperations(object):
    """Class to support remote operations."""

    def __init__(self,
                 user_host,
                 ssh_connection_options=None,
                 ssh_options=None,
                 scp_options=None,
                 retries=0,
                 retry_sleep=0,
                 debug=False,
                 shell_binary="/bin/bash",
                 use_shell=False):

        self.user_host = user_host
        self.ssh_connection_options = ssh_connection_options if ssh_connection_options else ""
        self.ssh_options = ssh_options if ssh_options else ""
        self.scp_options = scp_options if scp_options else ""
        self.retries = retries
        self.retry_sleep = retry_sleep
        self.debug = debug
        self.shell_binary = shell_binary
        self.use_shell = use_shell
        # Check if we can remotely access the host.
        self._access_code, self._access_buff = self._remote_access()

    def _call(self, cmd):
        if self.debug:
            print(cmd)
        # If use_shell is False we need to split the command up into a list.
        if not self.use_shell:
            cmd = shlex.split(cmd)
        # Use a common pipe for stdout & stderr for logging.
        process = subprocess.Popen(cmd,
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT,
                                   shell=self.use_shell)
        buff_stdout, _ = process.communicate()
        return process.poll(), buff_stdout

    def _remote_access(self):
        """ This will check if a remote session is possible. """
        cmd = "ssh {} {} {} date".format(
            self.ssh_connection_options, self.ssh_options, self.user_host)
        attempt_num = 0
        buff = ""
        while True:
            ret, buff = self._call(cmd)
            # Ignore any connection errors before sshd has fully initialized.
            if not ret and not any(ssh_error in buff for ssh_error in _SSH_CONNECTION_ERRORS):
                return ret, buff
            attempt_num += 1
            if attempt_num > self.retries:
                break
            if self.debug:
                print("Failed remote attempt {}, retrying in {} seconds".format(
                    attempt_num, self.retry_sleep))
            time.sleep(self.retry_sleep)
        return ret, buff

    def _perform_operation(self, cmd):
        return self._call(cmd)

    def access_established(self):
        """Return True if initial access was established."""
        return not self._access_code

    def access_info(self):
        """ Returns return code and output buffer from initial access attempt(s). """
        return self._access_code, self._access_buff

    @staticmethod
    def ssh_error(message):
        """Return True if the error message is generated from the ssh client.

        This can help determine if an error is due to a remote operation failing or an ssh
        related issue, like a connection issue.
        """
        return message.startswith("ssh:")

    def operation(  # pylint: disable=too-many-branches
            self, operation_type, operation_param, operation_dir=None):
        """Execute Main entry for remote operations. Returns (code, output).

            'operation_type' supports remote shell and copy operations.
            'operation_param' can either be a list or string of commands or files.
            'operation_dir' is '.' if unspecified for 'copy_*'.
        """

        if not self.access_established():
            return self.access_info()

        # File names with a space must be quoted, since we permit the
        # the file names to be either a string or a list.
        if operation_type.startswith("copy") and isinstance(operation_param, str):
            operation_param = shlex.split(operation_param, posix=not _IS_WINDOWS)

        cmds = []
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
            cmd = "ssh {} {} {} {} -c \"{}'{}'\"".format(
                self.ssh_connection_options,
                self.ssh_options,
                self.user_host,
                self.shell_binary,
                dollar,
                operation_param)
            cmds.append(cmd)

        elif operation_type == "copy_to":
            cmd = "scp -r {} {} ".format(self.ssh_connection_options, self.scp_options)
            # To support spaces in the filename or directory, we quote them one at a time.
            for copy_file in operation_param:
                # Quote file on Posix.
                quote = "\"" if not _IS_WINDOWS else ""
                cmd += "{quote}{file}{quote} ".format(quote=quote, file=posix_path(copy_file))
            operation_dir = operation_dir if operation_dir else ""
            cmd += " {}:{}".format(self.user_host, posix_path(operation_dir))
            cmds.append(cmd)

        elif operation_type == "copy_from":
            operation_dir = operation_dir if operation_dir else "."
            if not os.path.isdir(operation_dir):
                raise ValueError(
                    "Local directory '{}' does not exist.".format(operation_dir))

            # We support multiple files being copied from the remote host
            # by invoking scp for each file specified.
            # Note - this is a method which scp does not support directly.
            for copy_file in operation_param:
                copy_file = posix_path(copy_file)
                cmd = "scp -r {} {} {}:".format(
                    self.ssh_connection_options, self.scp_options, self.user_host)
                # Quote (on Posix), and escape the file if there are spaces.
                # Note - we do not support other non-ASCII characters in a file name.
                quote = "\"" if not _IS_WINDOWS else ""
                if " " in copy_file:
                    copy_file = re.escape("{quote}{file}{quote}".format(
                        quote=quote, file=copy_file))
                cmd += "{} {}".format(copy_file, posix_path(operation_dir))
                cmds.append(cmd)

        else:
            raise ValueError(
                "Invalid operation '{}' specified, choose from {}.".format(
                    operation_type, _OPERATIONS))

        final_ret = 0
        buff = ""
        for cmd in cmds:
            ret, new_buff = self._perform_operation(cmd)
            buff += new_buff
            final_ret = final_ret or ret

        return final_ret, buff

    def shell(self, operation_param, operation_dir=None):
        """ Helper for remote shell operations. """
        return self.operation(
            operation_type="shell",
            operation_param=operation_param,
            operation_dir=operation_dir)

    def copy_to(self, operation_param, operation_dir=None):
        """ Helper for remote copy_to operations. """
        return self.operation(
            operation_type="copy_to",
            operation_param=operation_param,
            operation_dir=operation_dir)

    def copy_from(self, operation_param, operation_dir=None):
        """ Helper for remote copy_from operations. """
        return self.operation(
            operation_type="copy_from",
            operation_param=operation_param,
            operation_dir=operation_dir)


def main():
    """ Main program. """

    parser = optparse.OptionParser(description=__doc__)
    control_options = optparse.OptionGroup(parser, "Control options")
    shell_options = optparse.OptionGroup(parser, "Shell options")
    copy_options = optparse.OptionGroup(parser, "Copy options")

    parser.add_option("--userHost",
                      dest="user_host",
                      default=None,
                      help="User and remote host to execute commands on [REQUIRED]."
                           " Examples, 'user@1.2.3.4' or 'user@myhost.com'.")

    parser.add_option("--operation",
                      dest="operation",
                      default="shell",
                      choices=_OPERATIONS,
                      help="Remote operation to perform, choose one of '{}',"
                           " defaults to '%default'.".format(", ".join(_OPERATIONS)))

    control_options.add_option("--sshConnectionOptions",
                               dest="ssh_connection_options",
                               default=None,
                               action="append",
                               help="SSH connection options which are common to ssh and scp."
                                    " More than one option can be specified either"
                                    " in one quoted string or by specifying"
                                    " this option more than once. Example options:"
                                    " '-i $HOME/.ssh/access.pem -o ConnectTimeout=10"
                                    " -o ConnectionAttempts=10'")

    control_options.add_option("--sshOptions",
                               dest="ssh_options",
                               default=None,
                               action="append",
                               help="SSH specific options."
                                    " More than one option can be specified either"
                                    " in one quoted string or by specifying"
                                    " this option more than once. Example options:"
                                    " '-t' or '-T'")

    control_options.add_option("--scpOptions",
                               dest="scp_options",
                               default=None,
                               action="append",
                               help="SCP specific options."
                                    " More than one option can be specified either"
                                    " in one quoted string or by specifying"
                                    " this option more than once. Example options:"
                                    " '-l 5000'")

    control_options.add_option("--retries",
                               dest="retries",
                               type=int,
                               default=0,
                               help="Number of retries to attempt for operation,"
                                    " defaults to '%default'.")

    control_options.add_option("--retrySleep",
                               dest="retry_sleep",
                               type=int,
                               default=10,
                               help="Number of seconds to wait between retries,"
                                    " defaults to '%default'.")

    control_options.add_option("--debug",
                               dest="debug",
                               action="store_true",
                               default=False,
                               help="Provides debug output.")

    control_options.add_option("--verbose",
                               dest="verbose",
                               action="store_true",
                               default=False,
                               help="Print exit status and output at end.")

    shell_options.add_option("--commands",
                             dest="remote_commands",
                             default=None,
                             action="append",
                             help="Commands to excute on the remote host. The"
                                  " commands must be separated by a ';' and can either"
                                  " be specifed in a quoted string or by specifying"
                                  " this option more than once. A ';' will be added"
                                  " between commands when this option is specifed"
                                  " more than once.")

    shell_options.add_option("--commandDir",
                             dest="command_dir",
                             default=None,
                             help="Working directory on remote to execute commands"
                                  " form. Defaults to remote login directory.")

    copy_options.add_option("--file",
                            dest="files",
                            default=None,
                            action="append",
                            help="The file to copy to/from remote host. To"
                                 " support spaces in the file, each file must be"
                                 " specified using this option more than once.")

    copy_options.add_option("--remoteDir",
                            dest="remote_dir",
                            default=None,
                            help="Remote directory to copy to, only applies when"
                                 " operation is 'copy_to'. Defaults to the login"
                                 " directory on the remote host.")

    copy_options.add_option("--localDir",
                            dest="local_dir",
                            default=".",
                            help="Local directory to copy to, only applies when"
                                 " operation is 'copy_from'. Defaults to the"
                                 " current directory, '%default'.")

    parser.add_option_group(control_options)
    parser.add_option_group(shell_options)
    parser.add_option_group(copy_options)

    (options, _) = parser.parse_args()

    if not getattr(options, "user_host", None):
        parser.print_help()
        parser.error("Missing required option")

    if options.operation == "shell":
        if not getattr(options, "remote_commands", None):
            parser.print_help()
            parser.error("Missing required '{}' option '{}'".format(
                options.operation, "--commands"))
        operation_param = ";".join(options.remote_commands)
        operation_dir = options.command_dir
    else:
        if not getattr(options, "files", None):
            parser.print_help()
            parser.error("Missing required '{}' option '{}'".format(
                options.operation, "--file"))
        operation_param = options.files
        if options.operation == "copy_to":
            operation_dir = options.remote_dir
        else:
            operation_dir = options.local_dir

    if not options.ssh_connection_options:
        ssh_connection_options = None
    else:
        ssh_connection_options = " ".join(options.ssh_connection_options)

    if not options.ssh_options:
        ssh_options = None
    else:
        ssh_options = " ".join(options.ssh_options)

    if not options.scp_options:
        scp_options = None
    else:
        scp_options = " ".join(options.scp_options)

    remote_op = RemoteOperations(
        user_host=options.user_host,
        ssh_connection_options=ssh_connection_options,
        ssh_options=ssh_options,
        scp_options=scp_options,
        retries=options.retries,
        retry_sleep=options.retry_sleep,
        debug=options.debug)
    ret_code, buffer = remote_op.operation(options.operation, operation_param, operation_dir)
    if options.verbose:
        print("Return code: {} for command {}".format(ret_code, sys.argv))
        print(buffer)

    sys.exit(ret_code)


if __name__ == "__main__":
    main()
