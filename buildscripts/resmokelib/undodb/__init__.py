"""Interactions with the undodb tool-suite."""
from typing import Optional

from buildscripts.resmokelib.plugin import PluginInterface, Subcommand

_HELP = """
Info on how to install undodb.
"""

_MESSAGE = """
You must manually download install the undodb package.

UndoDB is only supported on linux platforms. It will not work on macOS or Windows.

1. Download the tarball from Google Drive.

       https://drive.google.com/open?id=18dx1hHRrPgc27TtvvCe9rLXSYx_rKjzq

   You must be logged into your MongoDB/10gen google account to access this link.
   This file has MongoDB's private undodb key-server parameters baked into it,
   so do not share this file outside of the company.

2. Untar and install:

        tar xzf undodb-*.tgz
        cd undodb-*
        sudo make install

There is good README help in the undodb directory if you have questions.
There is also extensive documentation at https://docs.undo.io. 

Please also refer to William Schultz's intro talk for a getting started primer:

    https://mongodb.zoom.com/rec/share/5eBrDqHJ7k5If6uX9Fn7Wo0sGKT6T6a8gydK-_QOxBkaEyZzwv6Yf4tjTB4cS0f1

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
        return UndoDb()
