"""YAPF linter support module."""
from __future__ import absolute_import
from __future__ import print_function

from typing import List

from . import base


class YapfLinter(base.LinterBase):
    """Yapf linter."""

    def __init__(self):
        # type: () -> None
        """Create a yapf linter."""
        super(YapfLinter, self).__init__("yapf", "yapf 0.16.0")

    def get_lint_version_cmd_args(self):
        # type: () -> List[str]
        """Get the command to run a linter version check."""
        return ["--version"]

    def needs_file_diff(self):
        # type: () -> bool
        """See comment in base class."""
        return True

    def get_lint_cmd_args(self, file_name):
        # type: (str) -> List[str]
        """Get the command to run a linter."""
        return [file_name]

    def get_fix_cmd_args(self, file_name):
        # type: (str) -> List[str]
        """Get the command to run a linter fix."""
        return ["-i", file_name]
