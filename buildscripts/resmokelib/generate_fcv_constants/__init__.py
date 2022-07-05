"""Generate FCV constants for consumption by non-C++ integration tests."""
import argparse

from buildscripts.resmokelib import configure_resmoke
from buildscripts.resmokelib import logging
from buildscripts.resmokelib.plugin import PluginInterface, Subcommand

_COMMAND = "generate-fcv-constants"


class GenerateFCVConstants(Subcommand):
    """Interact with generating FCV constants."""

    def __init__(self):
        """Constructor."""
        self._exec_logger = None

    def _setup_logging(self):
        logging.loggers.configure_loggers()
        self._exec_logger = logging.loggers.ROOT_EXECUTOR_LOGGER

    def execute(self) -> None:
        """
        Work your magic.

        :return: None
        """
        # This will cause multiversion constants to be generated.
        self._setup_logging()

        import buildscripts.resmokelib.multiversionconstants  # pylint: disable=unused-import
        buildscripts.resmokelib.multiversionconstants.log_constants(self._exec_logger)


class GenerateFCVConstantsPlugin(PluginInterface):
    """Interact with generating FCV constants."""

    def add_subcommand(self, subparsers):
        """
        Add 'generate-fcv-constants' subcommand.

        :param subparsers: argparse parser to add to
        :return: None
        """
        # Can't hide this subcommand due to a Python bug. https://bugs.python.org/issue22848.
        subparsers.add_parser(_COMMAND, help=argparse.SUPPRESS)

    def parse(self, subcommand, parser, parsed_args, **kwargs):
        """
        Return the FCV constants subcommand for execution.

        :param subcommand: equivalent to parsed_args.command
        :param parser: parser used
        :param parsed_args: output of parsing
        :param kwargs: additional args
        :return: None or a Subcommand
        """
        configure_resmoke.validate_and_update_config(parser, parsed_args)
        if subcommand != _COMMAND:
            return None

        return GenerateFCVConstants()
