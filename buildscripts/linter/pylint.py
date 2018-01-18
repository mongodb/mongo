"""PyLint linter support module."""
from __future__ import absolute_import
from __future__ import print_function

import os
from typing import List

from . import base
from . import git


class PyLintLinter(base.LinterBase):
    """Pylint linter."""

    def __init__(self):
        # type: () -> None
        """Create a pylint linter."""
        self._rc_file = os.path.join(
            os.path.normpath(git.get_base_dir()), "buildscripts", ".pylintrc")
        super(PyLintLinter, self).__init__("pylint", "pylint 1.6.5")

    def get_lint_version_cmd_args(self):
        # type: () -> List[str]
        """Get the command to run a linter version check."""
        return ["--version"]

    def get_lint_cmd_args(self, file_name):
        # type: (str) -> List[str]
        """Get the command to run a linter."""
        # pylintrc only searches parent directories if it is a part of a module, and since our code
        # is split across different modules, and individual script files, we need to specify the
        # path to the rcfile.
        # See https://pylint.readthedocs.io/en/latest/user_guide/run.html
        return [
            "--rcfile=%s" % (self._rc_file), "--output-format", "msvs", "--reports=n", file_name
        ]
