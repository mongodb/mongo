"""Interactions with the resmoke bisect command."""

from buildscripts.resmokelib.plugin import PluginInterface, Subcommand

# TODO SERVER-59451 Complete the help messages and instructions.
_HELP = """
Info on how to use resmoke bisect.
"""

_MESSAGE = """Instructions for setting up and using Resmoke's bisect function
"""

_COMMAND = "bisect"


class Bisect(Subcommand):  # pylint: disable=invalid-name
    """Interact with Resmoke bisect."""

    def __init__(self, options):
        """
        Constructor.

        :param options: Options as parsed by parser.py
        """

    def execute(self) -> None:
        """
        Work your magic.

        :return: None
        """
        print(_MESSAGE)


class BisectPlugin(PluginInterface):
    """Interact with Resmoke bisect."""

    def add_subcommand(self, subparsers):
        """
        Add 'bisect' subcommand.

        :param subparsers: argparse parser to add to
        :return: None
        """
        parser = subparsers.add_parser(_COMMAND, help=_HELP)
        parser.add_argument("--lookback", '-l', action="store", type=int, default=365,
                            help="Maximum number of days to look back while bisecting commits.")

        # Accept arbitrary args like 'resmoke.py bisect foobar', but ignore them.
        parser.add_argument("args", nargs="*")

    def parse(self, subcommand, parser, parsed_args, **kwargs):
        """
        Return bisect if command is one we recognize.

        :param subcommand: equivalent to parsed_args.command
        :param parser: parser used
        :param parsed_args: output of parsing
        :param kwargs: additional args
        :return: None or a Subcommand
        """
        if subcommand != _COMMAND:
            return None

        return Bisect(parsed_args)
