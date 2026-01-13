"""Interactions with the undodb tool-suite."""

from buildscripts.resmokelib.plugin import PluginInterface, Subcommand

from . import fetch

_HELP = """
Info on how to install undodb.
"""

_MESSAGE = """Instructions for setting up and using UndoDB's reversible debugger

Setup and usage instructions have can be found at:

    https://wiki.corp.mongodb.com/display/KERNEL/UndoDB+Usage
"""

_COMMAND = "undodb"


class UndoDb(Subcommand):
    """Interact with UndoDB."""

    def __init__(self):
        """Constructor."""

    def execute(self) -> None:
        """
        Work your magic.

        :return: None
        """
        print(_MESSAGE)


class UndoDbPlugin(PluginInterface):
    """Interact with UndoDB."""

    def add_subcommand(self, subparsers):
        """
        Add 'undodb' subcommand.

        :param subparsers: argparse parser to add to
        :return: None
        """
        parser = subparsers.add_parser(_COMMAND, help=_HELP)
        # Accept arbitrary args like 'resmoke.py undodb foobar', but ignore them.
        parser.add_argument(
            "--fetch",
            "-f",
            action="store",
            type=str,
            help="Fetch UndoDB recordings archive with the given Evergreen task ID",
        )
        parser.add_argument("args", nargs="*")

    def parse(self, subcommand, parser, parsed_args, **kwargs):
        """
        Return UndoDb if command is one we recognize.

        :param subcommand: equivalent to parsed_args.command
        :param parser: parser used
        :param parsed_args: output of parsing
        :param kwargs: additional args
        :return: None or a Subcommand
        """
        if subcommand != _COMMAND:
            return None
        if parsed_args.fetch:
            return fetch.Fetch(parsed_args.fetch)

        return UndoDb()
