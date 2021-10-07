"""Interactions with the resmoke bisect command."""
import shutil
import subprocess
from datetime import datetime, timedelta, timezone

import structlog
from typing import List, Optional

from buildscripts.resmokelib.plugin import PluginInterface, Subcommand
from buildscripts.resmokelib.setup_multiversion.setup_multiversion import SetupMultiversion, \
    _DownloadOptions
from buildscripts.resmokelib.utils import evergreen_conn

# TODO SERVER-59451 Complete the help messages and instructions.

_HELP = """
Info on how to use resmoke bisect.
"""

_MESSAGE = """Instructions for setting up and using Resmoke's bisect function
"""

_COMMAND = "bisect"

LOGGER = structlog.getLogger(__name__)

TEST_ENV_FILEPATH = "test_env"


class Bisect(Subcommand):  # pylint: disable=invalid-name
    """Interact with Resmoke bisect."""

    def __init__(self, branch, lookback, variant, script, evergreen_config=None):
        """
        Constructor.

        :param options: Options as parsed by parser.py
        """
        self.branch = branch
        self.lookback = lookback
        self.variant = variant
        self.script = script
        self.evergreen_config = evergreen_config
        self.evg_api = evergreen_conn.get_evergreen_api(evergreen_config)

    def _clean_env(self, version):
        """Remove any directories added during testing process for the given version."""
        shutil.rmtree("build")
        shutil.rmtree(f"{TEST_ENV_FILEPATH}_{version}")

    def _setup_test_env(self, version):
        """Setup a test environment for the given version."""
        setup_test_env = SetupMultiversion(
            install_dir=f"{TEST_ENV_FILEPATH}_{version}",
            link_dir=f"{TEST_ENV_FILEPATH}_{version}",
            versions=[version],
            variant=self.variant,
            download_options=_DownloadOptions(db=True, ds=False, da=True, dv=True),
            evergreen_config=self.evergreen_config,
            ignore_failed_push=True
        )
        setup_test_env.execute()

    def _test_version_with_script(self, version):
        """Test the given version with the user provided script."""
        self._setup_test_env(version)
        status = subprocess.call(["sh", self.script])
        self._clean_env(version)
        return True if status == 0 else False

    def bisect(self, versions):
        """Bisect to find latest passing version assuming ordered by oldest to latest version."""
        if len(versions) == 0:
            return None
        midpoint = len(versions) // 2
        success = self._test_version_with_script(versions[midpoint].revision)
        # if success, keep checking right
        if success:
            return self.bisect(versions[midpoint + 1:]) or versions[midpoint]
        # if fail, keep checking left
        else:
            return self.bisect(versions[0:midpoint])

    def find_versions_with_binaries(self):
        """
        Find versions that have binaries for the user provided variant in the lookback period.

        :return: List of versions that have binaries ordered from earliest to latest.
        """
        versions_with_binaries = [
            version
            for version in self.evg_api.versions_by_project_time_window(
                project_id=f'mongodb-mongo-{"" if self.branch == "master" else "v"}{self.branch}',
                before=datetime.now(timezone.utc),
                after=datetime.now(timezone.utc)-timedelta(self.lookback)
            )
            if evergreen_conn.get_compile_artifact_urls(
                self.evg_api,
                version,
                self.variant,
                ignore_failed_push=True
            )
        ]
        versions_with_binaries.reverse()
        return versions_with_binaries

    def execute(self) -> List[Optional[str]]:
        """
        Perform bisect for the provided branch, variant & lookback period based on result of script.

        :return: The last known passing version and the first known failing version.
        """
        versions_with_binaries = self.find_versions_with_binaries()
        last_passing_version = self.bisect(versions_with_binaries)

        # Did not run bisect
        if not versions_with_binaries:
            return []

        # All failed
        if not last_passing_version:
            return [None, versions_with_binaries[0].revision]

        last_passing_version_idx = versions_with_binaries.index(last_passing_version)

        # All passed
        if last_passing_version_idx == len(versions_with_binaries) - 1:
            return [versions_with_binaries[last_passing_version_idx].revision, None]

        # Standard range
        return [
            versions_with_binaries[last_passing_version_idx].revision,
            versions_with_binaries[last_passing_version_idx + 1].revision
        ]


class BisectPlugin(PluginInterface):
    """Interact with Resmoke bisect."""

    def add_subcommand(self, subparsers):
        """
        Add 'bisect' subcommand.

        :param subparsers: argparse parser to add to
        :return: None
        """
        parser = subparsers.add_parser(_COMMAND, help=_HELP)
        parser.add_argument("--lookback", '-l', action="store", type=int, default=365,
                            help="Maximum number of days to look back while bisecting commits.")
        parser.add_argument("--branch", '-b', action="store", type=str, required=True,
                            help="The branch to bisect.")
        parser.add_argument("--variant", "-v", action="store", type=str, required=True,
                            help="Specify a build variant to use.")
        parser.add_argument("--script", "-s", action="store", type=str, required=True,
                            help="Location of the test script to run on the evergreen versions.")
        parser.add_argument(
            "-ec", "--evergreenConfig", dest="evergreen_config",
            help="Location of evergreen configuration file. If not specified it will look "
                 f"for it in the following locations: {evergreen_conn.EVERGREEN_CONFIG_LOCATIONS}")

        # Accept arbitrary args like 'resmoke.py bisect foobar', but ignore them.
        parser.add_argument("args", nargs="*")

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
            script=args.script
        )
