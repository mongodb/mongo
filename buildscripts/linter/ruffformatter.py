"""Ruff formatter support module."""

from . import base


class RuffFormatter(base.LinterBase):
    """Ruff formatter."""

    def __init__(self):
        """Create a Ruff formatter."""
        super(RuffFormatter, self).__init__("ruff", "0.4.4")

    def get_lint_version_cmd_args(self) -> list[str]:
        """Get the command to run a version check."""
        return ["--version"]

    def get_lint_cmd_args(self, files: list[str]) -> list[str]:
        """Get the command to run a check."""
        if not files:
            return ["format", "--check"]

        files = " ".join(files)
        return ["format", "--check", files]

    def get_fix_cmd_args(self, files: list[str]) -> list[str]:
        """Get the command to run a fix."""
        if not files:
            return ["format"]

        files = " ".join(files)
        return ["format", files]
