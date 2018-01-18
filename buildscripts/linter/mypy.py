"""Mypy linter support module."""
from __future__ import absolute_import
from __future__ import print_function

from typing import List

from . import base


class MypyLinter(base.LinterBase):
    """Mypy linter."""

    def __init__(self):
        # type: () -> None
        """Create a mypy linter."""
        super(MypyLinter, self).__init__("mypy", "mypy 0.501")

    def get_lint_version_cmd_args(self):
        # type: () -> List[str]
        """Get the command to run a linter version check."""
        return ["--version"]

    def get_lint_cmd_args(self, file_name):
        # type: (str) -> List[str]
        """Get the command to run a linter."""
        # -py2 - Check Python 2 code for type annotations in comments
        # --disallow-untyped-defs - Error if any code is missing type annotations
        # --ignore-missing-imports - Do not error if imports are not found. This can be a problem
        # with standalone scripts and relative imports. This will limit effectiveness but avoids
        # mypy complaining about running code.
        # --follow-imports=silent - Do not error on imported files since all imported files may not
        # be mypy clean
        return [
            "--py2", "--disallow-untyped-defs", "--ignore-missing-imports",
            "--follow-imports=silent", file_name
        ]

    def ignore_interpreter(self):
        # type: () -> bool
        # pylint: disable=no-self-use
        """
        Check if we should ignore the interpreter when searching for the linter to run.

        This applies to mypy specifically since the pylinters are executed under Python 2 but mypy
        is executed by python 3.
        """
        return True
