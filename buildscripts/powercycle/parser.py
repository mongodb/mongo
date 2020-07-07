"""Parser for command line arguments."""

import argparse

import buildscripts.powercycle.plugins as plugins

_PLUGINS = [plugins.PowercyclePlugin()]


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
    """Parse the command line arguments passed to powercycle_operations.py and return the subcommand object to execute."""
    parser, parsed_args = parse(sys_args)

    subcommand = parsed_args.command

    for plugin in _PLUGINS:
        subcommand_obj = plugin.parse(subcommand, parser, parsed_args, **kwargs)
        if subcommand_obj is not None:
            return subcommand_obj

    raise RuntimeError(
        f"Powercycle configuration has invalid subcommand: {subcommand}. Try '--help'")
