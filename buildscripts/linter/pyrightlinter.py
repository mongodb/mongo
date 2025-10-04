"""Pyright linter support module."""

from typing import List

from . import base


class PyrightLinter(base.LinterBase):
    """Pyright linter."""

    def __init__(self):
        """Create a Pyright linter."""
        super(PyrightLinter, self).__init__("pyright", "1.1.393")

    def get_lint_version_cmd_args(self) -> List[str]:
        """Get the command to run a version check."""
        return ["--version"]

    def get_lint_cmd_args(self, files: List[str]) -> List[str]:
        """Get the command to run a check."""
        return files if files else ["."]
