"""Class to support running various linters in a common framework."""

import logging
import os
import re
import site
import subprocess
import sys
import threading

import pkg_resources

from . import base


def _check_version(linter, cmd_path, args):
    # type: (base.LinterBase, List[str], List[str]) -> bool
    """Check if the given linter has the correct version."""

    try:
        cmd = cmd_path + args
        logging.info(str(cmd))
        process_handle = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        output, stderr = process_handle.communicate()
        decoded_output = output.decode("utf-8")

        if process_handle.returncode:
            logging.info(
                "Version check failed for [%s], return code '%d'."
                "Standard Output:\n%s\nStandard Error:\n%s",
                cmd,
                process_handle.returncode,
                decoded_output,
                stderr,
            )

        pattern = r"\b(?:(%s) )?(?P<version>\S+)\b" % (linter.cmd_name)
        required_version = pkg_resources.parse_version(linter.required_version)

        match = re.search(pattern, decoded_output)
        if match:
            found_version = match.group("version")
        else:
            found_version = "0.0"

        if pkg_resources.parse_version(found_version) < required_version:
            logging.info(
                "Linter %s has wrong version for '%s'. Expected >= '%s',"
                "Standard Output:\n'%s'\nStandard Error:\n%s",
                linter.cmd_name,
                cmd,
                required_version,
                decoded_output,
                stderr,
            )
            return False

    except OSError as os_error:
        # The WindowsError exception is thrown if the command is not found.
        # We catch OSError since WindowsError does not exist on non-Windows platforms.
        logging.info("Version check command [%s] failed: %s", cmd, os_error)
        return False

    return True


def _find_linter(linter, config_dict):
    # type: (base.LinterBase, Dict[str,str]) -> Optional[base.LinterInstance]
    """
    Look for a linter command with the required version.

    Return a LinterInstance with the location of the linter binary if a linter binary with the
    matching version is found. None otherwise.
    """

    if linter.cmd_name in config_dict and config_dict[linter.cmd_name] is not None:
        cmd = [config_dict[linter.cmd_name]]

        # If the user specified a tool location, we do not search any further
        if _check_version(linter, cmd, linter.get_lint_version_cmd_args()):
            return base.LinterInstance(linter, cmd)
        return None

    # Search for tool
    # 1. In the same directory as the interpreter
    # 2. Check user base -- i.e. site.USERBASE. With "pip install --user" puts files
    # 3. The current path
    # 4. In '/opt/mongodbtoolchain/v5/bin' if virtualenv is set up.
    python_dir = os.path.dirname(sys.executable)
    if sys.platform == "win32":
        # On Windows, these scripts are installed in %PYTHONDIR%\scripts like
        # 'C:\python\python310\scripts', and have .exe extensions.
        python_dir = os.path.join(python_dir, "scripts")

        cmd_str = os.path.join(python_dir, linter.cmd_name)
        cmd_str += ".exe"
        cmd = [cmd_str]
    else:
        # On Mac and with Homebrew, check for the binaries in /usr/local instead of sys.executable.
        if sys.platform == "darwin" and python_dir.startswith("/usr/local/opt"):
            python_dir = "/usr/local/bin"

        # On Linux, these scripts are installed in %PYTHONDIR%\bin like
        # '/opt/mongodbtoolchain/v5/bin', but they may point to the wrong interpreter.
        cmd_str = os.path.join(python_dir, linter.cmd_name)

        # Check for executables before assuming they are python scripts
        cmd = [cmd_str]
        if _check_version(linter, cmd, linter.get_lint_version_cmd_args()):
            return base.LinterInstance(linter, cmd)

        cmd = [sys.executable] + cmd

    # Check 1: interpreter location or for linters that ignore current interpreter.
    if _check_version(linter, cmd, linter.get_lint_version_cmd_args()):
        return base.LinterInstance(linter, cmd)

    logging.info(
        "First version check failed for linter '%s', trying a different location.", linter.cmd_name
    )

    # Check 2: Check USERBASE
    cmd = [os.path.join(site.getuserbase(), "bin", linter.cmd_name)]
    if _check_version(linter, cmd, linter.get_lint_version_cmd_args()):
        return base.LinterInstance(linter, cmd)

    # Check 3: current path
    cmd = [linter.cmd_name]
    if _check_version(linter, cmd, linter.get_lint_version_cmd_args()):
        return base.LinterInstance(linter, cmd)

    # Check 4: When a virtualenv is setup the linter modules are not installed, so we need
    # to use the linters installed in '/opt/mongodbtoolchain/v5/bin'.
    cmd = [sys.executable, os.path.join("/opt/mongodbtoolchain/v5/bin", linter.cmd_name)]
    if _check_version(linter, cmd, linter.get_lint_version_cmd_args()):
        return base.LinterInstance(linter, cmd)

    return None


def find_linters(linter_list, config_dict):
    # type: (List[base.LinterBase], Dict[str,str]) -> List[base.LinterInstance]
    """Find the location of all linters."""

    linter_instances = []  # type: List[base.LinterInstance]
    for linter in linter_list:
        linter_instance = _find_linter(linter, config_dict)
        if not linter_instance:
            logging.error(
                """\
Could not find the correct version of linter '%s', expected '%s'. Check your
PATH environment variable or re-run with --verbose for more information.

To fix, install the needed python modules for Python 3.x:
   python3 -m poetry install --no-root --sync

These commands are typically available via packages with names like python-pip,
or python3-pip. See your OS documentation for help.
""",
                linter.cmd_name,
                linter.required_version,
            )
            return None

        linter_instances.append(linter_instance)

    return linter_instances


class LintRunner(object):
    """Run a linter and print results in a thread safe manner."""

    def __init__(self):
        # type: () -> None
        """Create a Lint Runner."""
        self.print_lock = threading.Lock()

    def _safe_print(self, line):
        # type: (str) -> None
        """
        Print a line of text under a lock.

        Take a lock to ensure diffs do not get mixed when printed to the screen.
        """
        with self.print_lock:
            print(line)

    def run_lint(self, linter: base.LinterInstance, file_name: str, mongo_path: str) -> bool:
        """Run the specified linter for the file."""

        linter_args = linter.linter.get_lint_cmd_args(file_name)
        cmd = linter.cmd_path + linter_args

        logging.debug(" ".join(cmd))

        no_lint_errors = self.run(cmd)
        return no_lint_errors

    def run_fix(self, linter: base.LinterInstance, file_name: str, mongo_path: str) -> bool:
        """Run the specified lint fixes for the file."""

        linter_args = linter.linter.get_fix_cmd_args(file_name)

        cmd = linter.cmd_path + linter_args

        logging.debug(" ".join(cmd))

        no_lint_errors = self.run(cmd)
        return no_lint_errors

    def run(self, cmd):
        # type: (List[str]) -> bool
        """Check the specified cmd succeeds."""

        logging.debug(str(cmd))

        try:
            subprocess.check_output(cmd).decode("utf-8")
        except subprocess.CalledProcessError as cpe:
            self._safe_print("CMD [%s] failed:\n%s" % (" ".join(cmd), cpe.output.decode("utf-8")))
            return False

        return True
