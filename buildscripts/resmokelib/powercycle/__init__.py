"""Powercycle test.

Tests robustness of mongod to survive multiple powercycle events.

Client & server side powercycle test script.

This script is used in conjunction with certain Evergreen hosts
created with the `evergreen host create` command.
"""
import argparse

from buildscripts.resmokelib.plugin import PluginInterface, Subcommand
from buildscripts.resmokelib.powercycle import powercycle, powercycle_config, powercycle_constants
from buildscripts.resmokelib.powercycle.remote_hang_analyzer import RunHangAnalyzerOnRemoteInstance
from buildscripts.resmokelib.powercycle.save_diagnostics import GatherRemoteEventLogs, TarEC2Artifacts, \
    CopyEC2Artifacts, CopyEC2MonitorFiles, GatherRemoteMongoCoredumps, CopyRemoteMongoCoredumps
from buildscripts.resmokelib.powercycle.setup import SetUpEC2Instance

SUBCOMMAND = "powercycle"


class Powercycle(Subcommand):
    """Main class to run powercycle subcommand."""

    # Parser command Enum
    RUN = 1
    HOST_SETUP = 2
    SAVE_DIAG = 3
    REMOTE_HANG_ANALYZER = 4

    def __init__(self, parser_actions, options):
        """Initialize."""
        self.parser_actions = parser_actions
        self.options = options

    def execute(self):
        """Execute powercycle test."""
        return {
            self.RUN: self._exec_powercycle_main, self.HOST_SETUP: self._exec_powercycle_host_setup,
            self.SAVE_DIAG: self._exec_powercycle_save_diagnostics,
            self.REMOTE_HANG_ANALYZER: self._exec_powercycle_hang_analyzer
        }[self.options.run_option]()

    def _exec_powercycle_main(self):
        powercycle.main(self.parser_actions, self.options)

    @staticmethod
    def _exec_powercycle_host_setup():
        SetUpEC2Instance().execute()

    @staticmethod
    def _exec_powercycle_save_diagnostics():

        # The event logs on Windows are a useful diagnostic to have when determining if something bad
        # happened to the remote machine after it was repeatedly crashed during powercycle testing. For
        # example, the Application and System event logs have previously revealed that the mongod.exe
        # process abruptly exited due to not being able to open a file despite the process successfully
        # being restarted and responding to network requests.
        GatherRemoteEventLogs().execute()
        TarEC2Artifacts().execute()
        CopyEC2Artifacts().execute()
        CopyEC2MonitorFiles().execute()
        GatherRemoteMongoCoredumps().execute()
        CopyRemoteMongoCoredumps().execute()

    @staticmethod
    def _exec_powercycle_hang_analyzer():
        RunHangAnalyzerOnRemoteInstance().execute()


class PowercyclePlugin(PluginInterface):
    """Interface to parsing."""

    def __init__(self):
        """Initialize."""
        self.parser_actions = None

    @staticmethod
    def _add_powercycle_commands(parent_parser):
        """Add sub-subcommands for powercycle."""
        sub_parsers = parent_parser.add_subparsers(help="powercycle commands")

        setup_parser = sub_parsers.add_parser("setup-host")
        setup_parser.set_defaults(run_option=Powercycle.HOST_SETUP)

        save_parser = sub_parsers.add_parser("save-diagnostics")
        save_parser.set_defaults(run_option=Powercycle.SAVE_DIAG)

        save_parser = sub_parsers.add_parser("remote-hang-analyzer")
        save_parser.set_defaults(run_option=Powercycle.REMOTE_HANG_ANALYZER)

        run_parser = sub_parsers.add_parser("run")
        run_parser.set_defaults(run_option=Powercycle.RUN)

        # Only need to return run_parser for further processing; others don't need additional args.
        return run_parser

    def add_subcommand(self, subparsers):  # pylint: disable=too-many-statements
        """Create and add the parser for the subcommand."""
        intermediate_parser = subparsers.add_parser(
            SUBCOMMAND, help=__doc__,
            usage="MongoDB Powercycle tests; type one of the subcommands for more information")

        parser = self._add_powercycle_commands(intermediate_parser)

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
