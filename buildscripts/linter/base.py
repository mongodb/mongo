"""Base class and support functions for linters."""
from __future__ import absolute_import
from __future__ import print_function

from abc import ABCMeta, abstractmethod
from typing import Dict, List, Optional


class LinterBase(object):
    """Base Class for all linters."""

    __metaclass__ = ABCMeta

    def __init__(self, cmd_name, required_version):
        # type: (str, str) -> None
        """
        Create a linter.

        cmd_name - short friendly name
        required_version - the required version string to check against
        """
        self.cmd_name = cmd_name
        self.required_version = required_version

    @abstractmethod
    def get_lint_cmd_args(self, file_name):
        # type: (str) -> List[str]
        """Get the command to run a linter."""
        pass

    def get_fix_cmd_args(self, file_name):
        # type: (str) -> Optional[List[str]]
        # pylint: disable=no-self-use,unused-argument
        """Get the command to run a linter fix."""
        return None

    @abstractmethod
    def get_lint_version_cmd_args(self):
        # type: () -> List[str]
        """Get the command to run a linter version check."""
        pass

    def needs_file_diff(self):
        # type: () -> bool
        # pylint: disable=no-self-use
        """
        Check if we need to diff the output of this linter with the original file.

        This applies to tools like clang-format and yapf which do not have a notion of linting. We
        introduce the idea of linting by formatting a file with the tool to standard out and
        comparing it to the original.
        """
        return False

    def ignore_interpreter(self):
        # type: () -> bool
        # pylint: disable=no-self-use
        """
        Check if we should ignore the interpreter when searching for the linter to run.

        This applies to mypy specifically since the pylinters are executed under Python 2 but mypy
        is executed by python 3.
        """
        return False


class LinterInstance(object):
    """A pair of a Linter and the full path of the linter cmd to run."""

    def __init__(self, linter, cmd_path):
        # type: (LinterBase, List[str]) -> None
        """Construct a LinterInstance."""
        self.linter = linter
        self.cmd_path = cmd_path
