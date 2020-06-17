"""Interactions with the undodb tool-suite."""

from buildscripts.resmokelib.plugin import PluginInterface, Subcommand

_HELP = """
Info on how to install undodb.
"""

_MESSAGE = """
Instructions for setting up and using UndoDB's reversible debugger  

UndoDB is only supported on linux platforms. It will not work on macOS or Windows.

1. Download the tarball from Google Drive.

       https://drive.google.com/open?id=18dx1hHRrPgc27TtvvCe9rLXSYx_rKjzq

   You must be logged into your MongoDB/10gen google account to access this link.
   This file has MongoDB's private undodb key-server parameters baked into it,
   so do not share this file outside of the company.
   
   If you're using an Evergreen virtual workstation, use the following command
   to copy the tarball over.
       
       evergreen host rsync --host <HOST_ID> -l /path/to/local/undodb-*.tgz -r /home/ubuntu/undo.tgz

2. Untar and install:

        tar xzf undodb-*.tgz
        cd undodb-*
        sudo make install

There is good README help in the undodb directory if you have questions.

3. To use UndoDB, you first need to make a recording by running a test suite
   in resmoke.py with the recorder
        
        ./buildscripts/resmoke.py run --recordWith live-record [your other resmoke args]
    
    This will generate one recording per invocation of each mongod and mongos process.
    The recordings are stored in the current working directory.
    
    Once you have the recording, you're ready to use `udb` to start debugging:
    
        udb --undodb-gdb-exe /opt/mongodbtoolchain/gdb/bin/gdb path/to/recording.undo

    There is a quick reference guide of UndoDB commands at:
    https://undo.io/media/uploads/files/A5_UndoDB_quick_reference_guide_June_2019.pdf

    Please also refer to Will Schultz's intro talk for a getting started primer:

    https://mongodb.zoom.com/rec/share/5eBrDqHJ7k5If6uX9Fn7Wo0sGKT6T6a8gydK-_QOxBkaEyZzwv6Yf4tjTB4cS0f1
    
    If you have any questions, suggestions or run into hiccups, please reach out
    to #server-tig.

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
