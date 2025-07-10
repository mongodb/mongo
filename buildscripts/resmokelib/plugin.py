"""Interface for creating a resmoke plugin."""

import abc
import argparse


class Subcommand(object):
    """A resmoke subcommand to execute."""

    def execute(self):
        """Execute the subcommand."""
        raise NotImplementedError("execute must be implemented by Subcommand subclasses")


class PluginInterface(abc.ABC):
    """Subcommand/plugin interface."""

    def add_subcommand(self, subparsers):
        """
        Add parser options for this plugin.

        :param subparsers: argparse subparsers
        """
        raise NotImplementedError()

    def parse(
        self,
        subcommand: str,
        parser: argparse.ArgumentParser,
        parsed_args: dict,
        should_configure_otel: bool = True,
        **kwargs,
    ):
        """
        Resolve command-line options to a Subcommand or None.

        :param subcommand: equivalent to parsed_args.command
        :param parser: parser used
        :param parsed_args: output of parsing
        :param kwargs: additional args
        :return: None or a Subcommand
        """
        raise NotImplementedError()
