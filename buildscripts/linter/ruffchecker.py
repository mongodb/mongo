"""Ruff linter support module."""

from . import base


class RuffChecker(base.LinterBase):
    """Ruff linter."""

    def __init__(self):
        # type: () -> None
        """Create a Ruff linter."""
        super(RuffChecker, self).__init__("ruff", "0.4.4")

    def get_lint_version_cmd_args(self) -> list[str]:
        """Get the command to run a version check."""
        return ["--version"]

    def get_lint_cmd_args(self, files: list[str]) -> list[str]:
        """Get the command to run a check."""
        if not files:
            return ["check"]

        files = " ".join(files)
        return ["check", files]

    def get_fix_cmd_args(self, files: list[str]) -> list[str]:
        """Get the command to run a fix."""

        if not files:
            return ["check", "--fix"]

        files = " ".join(files)
        return ["check", "--fix", files]
