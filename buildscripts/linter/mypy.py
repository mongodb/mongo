"""Mypy linter support module."""

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
        # Only idl and linter should be type checked by mypy. Other
        # files return errors under python 3 type checking. If we
        # return an empty list the runner will skip this file.
        if 'idl' in file_name or 'linter' in file_name:
            return [file_name]
        return []
