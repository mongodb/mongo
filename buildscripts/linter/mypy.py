"""Mypy linter support module."""

import os

from . import base

MYPY_CONFIG_FILE = ".mypy.ini"


class MypyLinter(base.LinterBase):
    """Mypy linter."""

    def __init__(self):
        # type: () -> None
        """Create a mypy linter."""
        # User can override the location of mypy from an environment variable.

        super(MypyLinter, self).__init__("mypy", "1.3.0", os.getenv("MYPY"))

    def get_lint_version_cmd_args(self):
        # type: () -> List[str]
        """Get the command to run a linter version check."""
        return ["--version"]

    def get_lint_cmd_args(self, file_name):
        # type: (str) -> List[str]
        """Get the command to run a linter."""
        args = ["--config-file", MYPY_CONFIG_FILE]
        # Only idl and linter should be type checked by mypy. Other
        # files return errors under python 3 type checking. If we
        # return an empty list the runner will skip this file.
        if "idl" in file_name or "linter" in file_name:
            return args + [file_name]
        return []
