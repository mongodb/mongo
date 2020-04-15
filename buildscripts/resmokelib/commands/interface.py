"""Interface for creating a resmoke subcommand."""


class Subcommand(object):
    """A resmoke subcommand to execute."""

    def execute(self):
        """Execute the subcommand."""
        raise NotImplementedError("execue must be implemented by Subcommand subclasses")
