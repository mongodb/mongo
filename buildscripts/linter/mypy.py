"""Mypy linter support module."""
from __future__ import absolute_import
from __future__ import print_function

import os
from typing import List

from . import base


class MypyLinter(base.LinterBase):
    """Mypy linter."""

    def __init__(self):
        # type: () -> None
        """Create a mypy linter."""
        # User can override the location of mypy from an environment variable.

        super(MypyLinter, self).__init__("mypy", "mypy 0.580", os.getenv("MYPY"))

    def get_lint_version_cmd_args(self):
        # type: () -> List[str]
        """Get the command to run a linter version check."""
        return ["--version"]

    def get_lint_cmd_args(self, file_name):
        # type: (str) -> List[str]
        """Get the command to run a linter."""
        return [file_name]

    def ignore_interpreter(self):
        # type: () -> bool
        # pylint: disable=no-self-use
        """
        Check if we should ignore the interpreter when searching for the linter to run.

        This applies to mypy specifically since the pylinters are executed under Python 2 but mypy
        is executed by python 3.
        """
        return True
