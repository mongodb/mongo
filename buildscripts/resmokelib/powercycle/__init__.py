"""Powercycle test.

Tests robustness of mongod to survive multiple powercycle events.

Client & server side powercycle test script.

This script is used in conjunction with certain Evergreen hosts
created with the `evergreen host create` command.
"""
import argparse

from buildscripts.resmokelib.plugin import PluginInterface, Subcommand
from buildscripts.resmokelib.powercycle import powercycle, powercycle_config, powercycle_constants

SUBCOMMAND = "powercycle"


class Powercycle(Subcommand):
    """Main class to run powercycle subcommand."""

    def __init__(self, parser_actions, options):
        """Initialize."""
        self.parser_actions = parser_actions
        self.options = options

    def execute(self):
        """Execute powercycle test."""
        powercycle.main(self.parser_actions, self.options)


class PowercyclePlugin(PluginInterface):
    """Interface to parsing."""

    def __init__(self):
        """Initialize."""
        self.parser_actions = None

    def add_subcommand(self, subparsers):  # pylint: disable=too-many-statements
        """Create and add the parser for the subcommand."""
        parser = subparsers.add_parser(SUBCOMMAND, help=__doc__, usage="usage")

        test_options = parser.add_argument_group("Test Options")
        mongodb_options = parser.add_argument_group("MongoDB Options")
        mongod_options = parser.add_argument_group("mongod Options")
        program_options = parser.add_argument_group("Program Options")

        # Test options
        test_options.add_argument("--sshUserHost", dest="ssh_user_host",
                                  help="Remote server ssh user/host, i.e., user@host (REQUIRED)",
                                  required=True)

        test_options.add_argument(
            "--sshConnection", dest="ssh_connection_options",
            help="Remote server ssh additional connection options, i.e., '-i ident.pem'"
            " which are added to '{}'".format(powercycle_constants.DEFAULT_SSH_CONNECTION_OPTIONS),
            default=None)

        test_options.add_argument(
            "--taskName", dest="task_name",
            help=f"Powercycle task name. Based on this value additional"
            f" config values will be used from '{powercycle_config.POWERCYCLE_TASKS_CONFIG}'."
            f" [default: '%(default)s']", default="powercycle")

        # MongoDB options
        mongodb_options.add_argument(
            "--downloadUrl", dest="tarball_url",
            help="URL of tarball to test, if unspecifed latest tarball will be"
            " used", default="latest")

        # mongod options
        # The current host used to start and connect to mongod. Not meant to be specified
        # by the user.
        mongod_options.add_argument("--mongodHost", dest="host", help=argparse.SUPPRESS,
                                    default=None)

        # The current port used to start and connect to mongod. Not meant to be specified
        # by the user.
        mongod_options.add_argument("--mongodPort", dest="port", help=argparse.SUPPRESS, type=int,
                                    default=None)

        # Program options
        log_levels = ["debug", "info", "warning", "error"]
        program_options.add_argument(
            "--logLevel", dest="log_level", choices=log_levels,
            help="The log level. Accepted values are: {}."
            " [default: '%(default)s'].".format(log_levels), default="info")

        program_options.add_argument(
            "--logFile", dest="log_file",
            help="The destination file for the log output. Defaults to stdout.", default=None)

        # Remote options, include commands and options sent from client to server under test.
        # These are 'internal' options, not meant to be directly specifed.
        # More than one remote operation can be provided and they are specified in the program args.
        program_options.add_argument("--remoteOperation", dest="remote_operation",
                                     help=argparse.SUPPRESS, action="store_true", default=False)

        program_options.add_argument("--rsyncDest", dest="rsync_dest", nargs=2,
                                     help=argparse.SUPPRESS, default=None)

        parser.add_argument("remote_operations", nargs="*", help=argparse.SUPPRESS)

        self.parser_actions = parser._actions[1:-1]  # pylint: disable=protected-access

    def parse(self, subcommand, parser, parsed_args, **kwargs):
        """Parse command-line options."""

        if subcommand == SUBCOMMAND:
            return Powercycle(self.parser_actions, parsed_args)
        return None
