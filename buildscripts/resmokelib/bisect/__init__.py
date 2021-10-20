"""Interactions with the resmoke bisect command."""
import logging
import os
import subprocess
import sys
from datetime import datetime, timedelta, timezone

import structlog
from buildscripts.resmokelib.plugin import PluginInterface, Subcommand
from buildscripts.resmokelib.utils import evergreen_conn

LOGGER = structlog.getLogger(__name__)

_HELP = """
Perform an evergreen-aware git-bisect to find the 'last passing version' and 'first failing version' 
of mongo, with respect to a user provided shell script.
"""

_USAGE = """
The 'bisect' command lets a user specify a '--branch', '--variant' & '--lookback' period on which to 
perform a bisect. The user also provides a shell test '--script' which exits with status code 0 to
indicate a successful test. The command performs the following steps:

(1) Get all versions for the given '--branch', '--variant' & '--lookback' period from Evergreen.
(2) Filter the versions for versions that Evergreen has binaries and artifacts for.
(3) Find the 'middle' version. 
(4) Setup a test environment.
    - The 'build/resmoke-bisect' directory will have a sub directory -- 
    'build/resmoke-bisect/{version_id}' containing the git repo for this version.
    - The 'binaries' & 'artifacts' will also be downloaded to the directory named 
    'build/resmoke-bisect/{version_id}'.
    - Create a virtual environment at 'build/resmoke-bisect/bisect_venv' and 
    install packages for this version.
(5) Activate 'bisect_venv' & run the user provided shell script from within the 
'build/resmoke-bisect/{version_id}' directory. 
(6) Teardown the test environment.
(7) Repeat steps (3)-(6) on the left half, if (5) failed, or right half, if (5) succeeded.

This command will print the "Last Known Passing Version" & "First Known Failing Version".

NOTE: This 'bisect' command assumes a perfect partition between passing & failing versions.
ie: [Pass, Pass, Pass, Fail, Fail, Fail]
If there is not a perfect partition, try modifying the '--lookback' period or shell '--script'.
"""

_COMMAND = "bisect"

BISECT_DIR = os.path.dirname(os.path.abspath(__file__))
SETUP_TEST_ENV_SH = os.path.join(BISECT_DIR, "setup_test_env.sh")
TEARDOWN_TEST_ENV_SH = os.path.join(BISECT_DIR, "teardown_test_env.sh")
RUN_USER_SCRIPT_SH = os.path.join(BISECT_DIR, "run_user_script.sh")
RESMOKE_FILEPATH = os.path.join(os.path.dirname(os.path.dirname(BISECT_DIR)), "resmoke.py")


def setup_logging(debug=False):
    """Enable logging."""
    log_level = logging.DEBUG if debug else logging.INFO
    logging.basicConfig(
        format="[%(asctime)s - %(name)s - %(levelname)s] %(message)s",
        level=log_level,
        stream=sys.stdout,
    )
    structlog.configure(logger_factory=structlog.stdlib.LoggerFactory())


class Bisect(Subcommand):  # pylint: disable=invalid-name
    """Main class for the Bisect subcommand."""

    def __init__(
            self,
            branch,
            lookback,
            variant,
            script,
            python_installation,
            evergreen_config=None,
            debug=None,
    ):
        """Initialize."""
        setup_logging(debug)
        self.branch = branch
        self.lookback = lookback
        self.variant = variant
        self.script = script
        self.python_installation = python_installation
        self.evergreen_config = evergreen_config
        self.evg_api = evergreen_conn.get_evergreen_api(evergreen_config)

    @staticmethod
    def _teardown_test_env(version):
        """Remove any directories added during testing process."""
        try:
            subprocess.run(["bash", TEARDOWN_TEST_ENV_SH], check=True)
        except subprocess.CalledProcessError as err:
            LOGGER.error("Could not teardown test environment for bisect.", version=version)
            raise err
        LOGGER.info("Completed teardown of test environment for bisect.", version=version)

    def _setup_test_env(self, version):
        """Set up a test environment for the given version."""
        try:
            subprocess.run(
                [
                    "bash",
                    SETUP_TEST_ENV_SH,
                    self.python_installation,
                    RESMOKE_FILEPATH,
                    self.evergreen_config if self.evergreen_config else "",
                    self.variant,
                    version,
                ],
                check=True,
            )
        except subprocess.CalledProcessError:
            LOGGER.error("Could not setup test environment for bisect -- retrying.",
                         version=version)
            try:
                subprocess.run(
                    [
                        "bash",
                        SETUP_TEST_ENV_SH,
                        self.python_installation,
                        RESMOKE_FILEPATH,
                        self.evergreen_config if self.evergreen_config else "",
                        self.variant,
                        version,
                    ],
                    check=True,
                )
            except subprocess.CalledProcessError as err:
                LOGGER.error("Could not setup test environment for bisect.", version=version)
                raise err
        LOGGER.info("Completed setup of test environment for bisect.", version=version)

    def _run_user_script(self, version):
        """Run the user script in a virtual environment."""
        return subprocess.run(["bash", RUN_USER_SCRIPT_SH, version, self.script],
                              check=False).returncode

    def _test_version_with_script(self, version):
        """Test the given version with the user provided script."""
        self._setup_test_env(version)
        success = self._run_user_script(version)
        self._teardown_test_env(version)
        return success == 0

    def bisect(self, versions):
        """Bisect to find latest passing version assuming ordered by oldest to latest version."""
        if len(versions) == 0:
            return None
        midpoint = len(versions) // 2
        success = self._test_version_with_script(versions[midpoint])
        # if success, keep checking right
        if success:
            LOGGER.info(
                "Version passed user script.",
                version=versions[midpoint],
            )
            return self.bisect(versions[midpoint + 1:]) or versions[midpoint]
        # if fail, keep checking left
        else:
            LOGGER.info("Version failed user script.", version=versions[midpoint])
            return self.bisect(versions[0:midpoint])

    def find_versions_with_binaries(self):
        """
        Find versions that have binaries for the user provided variant in the lookback period.

        :return: List of versions that have binaries ordered from earliest to latest.
        """
        versions_with_binaries = []
        for version in self.evg_api.versions_by_project_time_window(
                project_id=f'mongodb-mongo-{"" if self.branch == "master" else "v"}{self.branch}',
                before=datetime.now(timezone.utc),
                after=datetime.now(timezone.utc) - timedelta(self.lookback),
        ):
            urls = evergreen_conn.get_compile_artifact_urls(self.evg_api, version, self.variant,
                                                            ignore_failed_push=True)
            if urls.get("Artifacts") and urls.get("Binaries"):
                versions_with_binaries.append(version.revision)

        versions_with_binaries.reverse()
        return versions_with_binaries

    def execute(self):
        """
        Perform bisect for the provided branch, variant & lookback period based on result of script.

        Print the last passing version and first failing version.
        """
        versions_with_binaries = self.find_versions_with_binaries()

        # No versions found
        if not versions_with_binaries:
            LOGGER.info(
                "No versions with binaries found for given branch, variant & lookback period.",
                branch=self.branch,
                variant=self.variant,
                lookback=self.lookback,
            )
            exit(0)

        LOGGER.info("Performing bisect on the following versions: ",
                    versions=versions_with_binaries)

        last_passing_version = self.bisect(versions_with_binaries)
        first_failing_version = None

        # All versions failed
        if last_passing_version is None:
            first_failing_version = versions_with_binaries[0]
            LOGGER.info("All versions in lookback period failed.")

        # All versions passed
        elif last_passing_version == versions_with_binaries[-1]:
            LOGGER.info("All versions in lookback period passed.")

        else:
            first_failing_version = versions_with_binaries[
                versions_with_binaries.index(last_passing_version) + 1]

        print(f"Last Known Passing Version: {last_passing_version}")
        print(f"First Known Failing Version: {first_failing_version}")


class BisectPlugin(PluginInterface):
    """Interact with Resmoke bisect."""

    @classmethod
    def _add_args_to_parser(cls, parser):
        parser.add_argument(
            "--lookback",
            "-l",
            action="store",
            type=int,
            default=365,
            help="Maximum number of days to look back for versions to test.",
        )
        parser.add_argument(
            "--branch",
            "-b",
            action="store",
            type=str,
            required=True,
            help="The branch for which versions are being tested. [REQUIRED]",
        )
        parser.add_argument(
            "--variant",
            "-v",
            action="store",
            type=str,
            required=True,
            help="The variant for which versions are being tested. [REQUIRED]",
        )
        parser.add_argument(
            "--script",
            "-s",
            action="store",
            type=str,
            required=True,
            help="Location of the shell test script to run on the versions. [REQUIRED]",
        )
        parser.add_argument(
            "--python-installation",
            "-p",
            action="store",
            type=str,
            dest="python_installation",
            default="/opt/mongodbtoolchain/v3/bin/python3",
            required=False,
            help="Location of a python installation to use for shell commands. If not specified, "
            "it will use '/opt/mongodbtoolchain/v3/bin/python3' -- assuming this is being run on "
            "an Evergreen host. If this is being run from within a virtual env you can use "
            "'python' or 'python3'.",
        )
        parser.add_argument(
            "-ec",
            "--evergreenConfig",
            dest="evergreen_config",
            help="Location of evergreen configuration file. If not specified it will look "
            f"for it in the following locations: {evergreen_conn.EVERGREEN_CONFIG_LOCATIONS}",
        )
        parser.add_argument(
            "-d",
            "--debug",
            dest="debug",
            action="store_true",
            default=False,
            help="Set DEBUG logging level.",
        )

    def add_subcommand(self, subparsers):
        """
        Add 'bisect' subcommand.

        :param subparsers: argparse parser to add to.
        """
        parser = subparsers.add_parser(_COMMAND, usage=_USAGE, help=_HELP)
        self._add_args_to_parser(parser)

    def parse(self, subcommand, parser, parsed_args, **kwargs):
        """
        Return bisect if command is one we recognize.

        :param subcommand: equivalent to parsed_args.command
        :param parser: parser used
        :param parsed_args: output of parsing
        :param kwargs: additional args
        :return: None or a Subcommand
        """
        if subcommand != _COMMAND:
            return None
        args = parsed_args
        return Bisect(
            branch=args.branch,
            lookback=args.lookback,
            evergreen_config=args.evergreen_config,
            variant=args.variant,
            script=args.script,
            debug=args.debug,
            python_installation=args.python_installation,
        )
