"""Parser for command line arguments."""

import argparse
import shlex

from buildscripts.resmokelib import configure_resmoke
from buildscripts.resmokelib.hang_analyzer import HangAnalyzerPlugin
from buildscripts.resmokelib.powertest import PowertestPlugin
from buildscripts.resmokelib.run import RunPlugin
from buildscripts.resmokelib.setup_multiversion import SetupMultiversionPlugin
from buildscripts.resmokelib.undodb import UndoDbPlugin

_PLUGINS = [
    RunPlugin(),
    HangAnalyzerPlugin(),
    UndoDbPlugin(),
    SetupMultiversionPlugin(),
    PowertestPlugin(),
]


def _add_subcommands():
    """Create and return the command line arguments parser."""
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command")

    # Add sub-commands.
    for plugin in _PLUGINS:
        plugin.add_subcommand(subparsers)

    return parser


def parse(sys_args):
    """Parse the CLI args."""

    # Split out this function for easier testing.
    parser = _add_subcommands()
    parsed_args = parser.parse_args(sys_args)

    return parser, parsed_args


def parse_command_line(sys_args, **kwargs):
    """Parse the command line arguments passed to resmoke.py and return the subcommand object to execute."""
    parser, parsed_args = parse(sys_args)

    subcommand = parsed_args.command

    for plugin in _PLUGINS:
        subcommand_obj = plugin.parse(subcommand, parser, parsed_args, **kwargs)
        if subcommand_obj is not None:
            return subcommand_obj

    raise RuntimeError(f"Resmoke configuration has invalid subcommand: {subcommand}. Try '--help'")


def set_run_options(argstr=''):
    """Populate the config module variables for the 'run' subcommand with the default options."""
    parser, parsed_args = parse(['run'] + shlex.split(argstr))
    configure_resmoke.validate_and_update_config(parser, parsed_args)
