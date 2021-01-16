"""Powercycle test.

Tests robustness of mongod to survive multiple powercycle events.

Client & server side powercycle test script.

This script can be run against any host which is reachable via ssh.
Note - the remote hosts should be running bash shell (this script may fail otherwise).
There are no assumptions on the server what is the current deployment of MongoDB.
For Windows the assumption is that Cygwin is installed.
The server needs these utilities:
    - python 3.7 or higher,
    - sshd,
    - rsync.
This script will either download a MongoDB tarball or use an existing setup.
"""
import argparse
import distutils.spawn
import os

from buildscripts.resmokelib.plugin import PluginInterface, Subcommand
from buildscripts.resmokelib.powertest import powertest
from buildscripts.resmokelib.powertest import powercycle_constants

SUBCOMMAND = "powertest"


class Powertest(Subcommand):
    """Main class to run powertest subcommand."""

    def __init__(self, parser, parser_actions, options):
        """Initialize."""
        self.parser = parser
        self.parser_actions = parser_actions
        self.options = options

    def execute(self):
        """Execute powercycle test."""
        powertest.main(self.parser, self.parser_actions, self.options)


class PowertestPlugin(PluginInterface):
    """Interface to parsing."""

    def __init__(self):
        """Initialize."""
        self.parser_actions = None

    def add_subcommand(self, subparsers):  # pylint: disable=too-many-statements
        """Create and add the parser for the subcommand."""
        parser = subparsers.add_parser(SUBCOMMAND, help=__doc__)

        test_options = parser.add_argument_group("Test Options")
        crash_options = parser.add_argument_group("Crash Options")
        mongodb_options = parser.add_argument_group("MongoDB Options")
        mongod_options = parser.add_argument_group("mongod Options")
        client_options = parser.add_argument_group("Client Options")
        program_options = parser.add_argument_group("Program Options")

        # Test options
        test_options.add_argument("--sshUserHost", dest="ssh_user_host",
                                  help="Server ssh user/host, i.e., user@host (REQUIRED)",
                                  default=None)

        test_options.add_argument(
            "--sshConnection", dest="ssh_connection_options",
            help="Server ssh additional connection options, i.e., '-i ident.pem'"
            " which are added to '{}'".format(powercycle_constants.DEFAULT_SSH_CONNECTION_OPTIONS),
            default=None)

        test_options.add_argument("--testLoops", dest="num_loops",
                                  help="Number of powercycle loops to run [default: %(default)s]",
                                  type=int, default=10)

        test_options.add_argument("--testTime", dest="test_time",
                                  help="Time to run test (in seconds), overrides --testLoops",
                                  type=int, default=0)

        test_options.add_argument(
            "--seedDocNum", dest="seed_doc_num",
            help="Number of documents to seed the default collection [default:"
            " %(default)s]", type=int, default=0)

        test_options.add_argument(
            "--writeConcern", dest="write_concern",
            help="mongo (shell) CRUD client writeConcern, i.e.,"
            " '{\"w\": \"majority\"}' [default: '%(default)s']", default="{}")

        test_options.add_argument(
            "--readConcernLevel", dest="read_concern_level",
            help="mongo (shell) CRUD client readConcernLevel, i.e.,"
            "'majority'", default=None)

        # Crash options
        crash_methods = ["internal", "kill"]
        crash_options.add_argument(
            "--crashMethod", dest="crash_method", choices=crash_methods,
            help="Crash methods: {} [default: '%(default)s']."
            " Select 'internal' to crash the remote server through an"
            " internal command, i.e., sys boot (Linux) or notmyfault (Windows)."
            " Select 'kill' to perform an unconditional kill of mongod,"
            " which will keep the remote server running.".format(crash_methods), default="internal")

        # MongoDB options
        mongodb_options.add_argument(
            "--downloadUrl", dest="tarball_url",
            help="URL of tarball to test, if unspecifed latest tarball will be"
            " used", default="latest")

        # mongod options
        mongod_options.add_argument(
            "--replSet", dest="repl_set",
            help="Name of mongod single node replica set, if unpsecified mongod"
            " defaults to standalone node", default=None)

        # The current host used to start and connect to mongod. Not meant to be specified
        # by the user.
        mongod_options.add_argument("--mongodHost", dest="host", help=argparse.SUPPRESS,
                                    default=None)

        # The current port used to start and connect to mongod. Not meant to be specified
        # by the user.
        mongod_options.add_argument("--mongodPort", dest="port", help=argparse.SUPPRESS, type=int,
                                    default=None)

        mongod_options.add_argument("--mongodOptions", dest="mongod_options",
                                    help="Additional mongod options", default="")

        mongod_options.add_argument("--fcv", dest="fcv_version",
                                    help="Set the FeatureCompatibilityVersion of mongod.",
                                    default=None)

        # Client options
        client_options.add_argument(
            "--numCrudClients", dest="num_crud_clients",
            help="The number of concurrent CRUD clients to run"
            " [default: '%(default)s'].", type=int, default=1)

        client_options.add_argument(
            "--numFsmClients", dest="num_fsm_clients",
            help="The number of concurrent FSM clients to run"
            " [default: '%(default)s'].", type=int, default=0)

        # Program options
        program_options.add_argument(
            "--configFile", dest="config_file", help="YAML configuration file of program options."
            " Option values are mapped to command line option names."
            " The command line option overrides any specified options"
            " from this file.", default=None)

        program_options.add_argument(
            "--saveConfigOptions", dest="save_config_options",
            help="Save the program options to a YAML configuration file."
            " If this options is specified the program only saves"
            " the configuration file and exits.", default=None)

        program_options.add_argument(
            "--remotePython", dest="remote_python",
            help="The python intepreter to use on the remote host"
            " [default: '%(default)s']."
            " To be able to use a python virtual environment,"
            " which has already been provisioned on the remote"
            " host, specify something similar to this:"
            " 'source venv/bin/activate;  python'", default="python")

        program_options.add_argument(
            "--remoteSudo", dest="remote_sudo",
            help="Use sudo on the remote host for priveleged operations."
            " [default: %(default)s]."
            " For non-Windows systems, in order to perform privileged"
            " operations on the remote host, specify this, if the"
            " remote user is not able to perform root operations.", action="store_true",
            default=False)

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

        parser.add_argument("remote_operations", nargs="*",
                            help="Remote operations to execute on the remote host")

        self.parser_actions = parser._actions[1:-1]  # pylint: disable=protected-access

    def parse(self, subcommand, parser, parsed_args, **kwargs):
        """Parse command-line options."""

        if subcommand == SUBCOMMAND:
            return Powertest(parser, self.parser_actions, parsed_args)
        return None
