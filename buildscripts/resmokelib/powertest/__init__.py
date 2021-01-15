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
            " which are added to '{}'".format(powertest.DEFAULT_SSH_CONNECTION_OPTIONS),
            default=None)

        test_options.add_argument("--testLoops", dest="num_loops",
                                  help="Number of powercycle loops to run [default: %(default)s]",
                                  type=int, default=10)

        test_options.add_argument("--testTime", dest="test_time",
                                  help="Time to run test (in seconds), overrides --testLoops",
                                  type=int, default=0)

        test_options.add_argument("--rsync", dest="rsync_data",
                                  help="Rsync data directory between mongod stop and start",
                                  action="store_true", default=False)

        test_options.add_argument("--rsyncExcludeFiles", dest="rsync_exclude_files",
                                  help="Files excluded from rsync of the data directory",
                                  action="append", default=None)

        test_options.add_argument(
            "--backupPathBefore", dest="backup_path_before",
            help="Path where the db_path is backed up before crash recovery,"
            " defaults to '<rootDir>/data-beforerecovery'", default=None)

        test_options.add_argument(
            "--backupPathAfter", dest="backup_path_after",
            help="Path where the db_path is backed up after crash recovery,"
            " defaults to '<rootDir>/data-afterrecovery'", default=None)

        validate_locations = ["local", "remote"]
        test_options.add_argument(
            "--validate", dest="validate_collections",
            help="Run validate on all collections after mongod restart after"
            " a powercycle. Choose from {} to specify where the"
            " validate runs.".format(validate_locations), choices=validate_locations, default=None)

        canary_locations = ["local", "remote"]
        test_options.add_argument(
            "--canary", dest="canary",
            help="Generate and validate canary document between powercycle"
            " events. Choose from {} to specify where the canary is"
            " generated from. If the 'crashMethod' is not 'internal"
            " then this option must be 'local'.".format(canary_locations), choices=canary_locations,
            default=None)

        test_options.add_argument("--docForCanary", dest="canary_doc", help=argparse.SUPPRESS,
                                  default="")

        test_options.add_argument(
            "--seedDocNum", dest="seed_doc_num",
            help="Number of documents to seed the default collection [default:"
            " %(default)s]", type=int, default=0)

        test_options.add_argument("--dbName", dest="db_name", help=argparse.SUPPRESS,
                                  default="power")

        test_options.add_argument("--collectionName", dest="collection_name",
                                  help=argparse.SUPPRESS, default="cycle")

        test_options.add_argument(
            "--writeConcern", dest="write_concern",
            help="mongo (shell) CRUD client writeConcern, i.e.,"
            " '{\"w\": \"majority\"}' [default: '%(default)s']", default="{}")

        test_options.add_argument(
            "--readConcernLevel", dest="read_concern_level",
            help="mongo (shell) CRUD client readConcernLevel, i.e.,"
            "'majority'", default=None)

        # Crash options
        crash_methods = ["internal", "kill", "mpower"]
        crash_options.add_argument(
            "--crashMethod", dest="crash_method", choices=crash_methods,
            help="Crash methods: {} [default: '%(default)s']."
            " Select 'internal' to crash the remote server through an"
            " internal command, i.e., sys boot (Linux) or notmyfault (Windows)."
            " Select 'kill' to perform an unconditional kill of mongod,"
            " which will keep the remote server running."
            " Select 'mpower' to use the mFi mPower to cutoff power to"
            " the remote server.".format(crash_methods), default="internal")

        crash_options.add_argument(
            "--crashOption", dest="crash_option",
            help="Secondary argument for the following --crashMethod:"
            " 'mpower': specify output<num> to turn"
            " off/on, i.e., 'output1' (REQUIRED)."
            " 'internal': for Windows, optionally specify a crash method,"
            " i.e., 'notmyfault/notmyfaultc64.exe"
            " -accepteula crash 1'", default=None)

        crash_options.add_argument(
            "--crashWaitTime", dest="crash_wait_time",
            help="Time, in seconds, to wait before issuing crash [default:"
            " %(default)s]", type=int, default=30)

        crash_options.add_argument(
            "--jitterForCrashWaitTime", dest="crash_wait_time_jitter",
            help="The maximum time, in seconds, to be added to --crashWaitTime,"
            " as a uniform distributed random value, [default: %(default)s]", type=int, default=10)

        crash_options.add_argument("--sshCrashUserHost", dest="ssh_crash_user_host",
                                   help="The crash host's user@host for performing the crash.",
                                   default=None)

        crash_options.add_argument(
            "--sshCrashOption", dest="ssh_crash_option",
            help="The crash host's ssh connection options, i.e., '-i ident.pem'", default=None)

        # MongoDB options
        mongodb_options.add_argument(
            "--downloadUrl", dest="tarball_url",
            help="URL of tarball to test, if unspecifed latest tarball will be"
            " used", default="latest")

        mongodb_options.add_argument(
            "--rootDir", dest="root_dir",
            help="Root directory, on remote host, to install tarball and data"
            " directory [default: 'mongodb-powertest-<epochSecs>']", default=None)

        mongodb_options.add_argument(
            "--mongodbBinDir", dest="mongodb_bin_dir",
            help="Directory, on remote host, containing mongoDB binaries,"
            " overrides bin from tarball in --downloadUrl", default=None)

        mongodb_options.add_argument(
            "--dbPath", dest="db_path", help="Data directory to use, on remote host, if unspecified"
            " it will be '<rootDir>/data/db'", default=None)

        mongodb_options.add_argument(
            "--logPath", dest="log_path", help="Log path, on remote host, if unspecified"
            " it will be '<rootDir>/log/mongod.log'", default=None)

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

        # The ports used on the 'server' side when in standard or secret mode.
        mongod_options.add_argument(
            "--mongodUsablePorts", dest="usable_ports", nargs=2,
            help="List of usable ports to be used by mongod for"
            " standard and secret modes, [default: %(default)s]", type=int, default=[27017, 37017])

        mongod_options.add_argument("--mongodOptions", dest="mongod_options",
                                    help="Additional mongod options", default="")

        mongod_options.add_argument("--fcv", dest="fcv_version",
                                    help="Set the FeatureCompatibilityVersion of mongod.",
                                    default=None)

        mongod_options.add_argument(
            "--removeLockFile", dest="remove_lock_file",
            help="If specified, the mongod.lock file will be deleted after a"
            " powercycle event, before mongod is started.", action="store_true", default=False)

        # Client options
        mongo_path = distutils.spawn.find_executable("dist-test/bin/mongo",
                                                     os.getcwd() + os.pathsep + os.environ["PATH"])
        client_options.add_argument(
            "--mongoPath", dest="mongo_path",
            help="Path to mongo (shell) executable, if unspecifed, mongo client"
            " is launched from the current directory.", default=mongo_path)

        client_options.add_argument(
            "--mongoRepoRootDir", dest="mongo_repo_root_dir",
            help="Root directory of mongoDB repository, defaults to current"
            " directory.", default=None)

        client_options.add_argument(
            "--crudClient", dest="crud_client",
            help="The path to the CRUD client script on the local host"
            " [default: '%(default)s'].", default="jstests/hooks/crud_client.js")

        with_external_server = "buildscripts/resmokeconfig/suites/with_external_server.yml"
        client_options.add_argument(
            "--configCrudClient", dest="config_crud_client",
            help="The path to the CRUD client configuration YML file on the"
            " local host. This is the resmoke.py suite file. If unspecified,"
            " a default configuration YML file (%(default)s) will be used that"
            " provides a mongo (shell) DB connection to a running mongod.",
            default=with_external_server)

        client_options.add_argument(
            "--numCrudClients", dest="num_crud_clients",
            help="The number of concurrent CRUD clients to run"
            " [default: '%(default)s'].", type=int, default=1)

        client_options.add_argument(
            "--numFsmClients", dest="num_fsm_clients",
            help="The number of concurrent FSM clients to run"
            " [default: '%(default)s'].", type=int, default=0)

        client_options.add_argument(
            "--fsmWorkloadFiles", dest="fsm_workload_files",
            help="A list of the FSM workload files to execute. More than one"
            " file can be specified either in a comma-delimited string,"
            " or by specifying this option more than once. If unspecified,"
            " then all FSM workload files are executed.", action="append", default=[])

        client_options.add_argument(
            "--fsmWorkloadBlacklistFiles", dest="fsm_workload_blacklist_files",
            help="A list of the FSM workload files to blacklist. More than one"
            " file can be specified either in a comma-delimited string,"
            " or by specifying this option more than once. Note the"
            " file name is the basename, i.e., 'distinct.js'.", action="append", default=[])

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
            "--reportJsonFile", dest="report_json_file",
            help="Create or update the specified report file upon program"
            " exit.", default=None)

        program_options.add_argument(
            "--exitYamlFile", dest="exit_yml_file",
            help="If specified, create a YAML file on exit containing"
            " exit code.", default=None)

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

        program_options.add_argument("--version", dest="version",
                                     help="Display this program's version", action="store_true",
                                     default=False)

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
