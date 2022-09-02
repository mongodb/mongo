"""Base class and support functions for linters."""

from abc import ABCMeta, abstractmethod
from typing import Dict, List, Optional


class LinterBase(object, metaclass=ABCMeta):
    """Base Class for all linters."""

    def __init__(self, cmd_name, required_version, cmd_location=None):
        # type: (str, str, Optional[str]) -> None
        """
        Create a linter.

        cmd_name - short friendly name
        required_version - the required version string to check against
        cmd_location - location of executable
        """
        self.cmd_name = cmd_name
        self.required_version = required_version
        self.cmd_location = cmd_location

    @abstractmethod
    def get_lint_cmd_args(self, file_name):
        # type: (str) -> List[str]
        """Get the command to run a linter."""
        pass

    def get_fix_cmd_args(self, _file_name):
        # type: (str) -> Optional[List[str]]
        """Get the command to run a linter fix."""
        return None

    @abstractmethod
    def get_lint_version_cmd_args(self):
        # type: () -> List[str]
        """Get the command to run a linter version check."""
        pass

    def needs_file_diff(self):
        # type: () -> bool
        """
        Check if we need to diff the output of this linter with the original file.

        This applies to tools like clang-format and yapf which do not have a notion of linting. We
        introduce the idea of linting by formatting a file with the tool to standard out and
        comparing it to the original.
        """
        return False


class LinterInstance(object):
    """A pair of a Linter and the full path of the linter cmd to run."""

    def __init__(self, linter, cmd_path):
        # type: (LinterBase, List[str]) -> None
        """Construct a LinterInstance."""
        self.linter = linter
        self.cmd_path = cmd_path
