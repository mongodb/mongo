"""Wrapper around mongosym to download everything required."""
import argparse
import logging
import os
import shutil
import subprocess
import sys

import structlog

from buildscripts import mongosymb
from buildscripts.resmokelib.plugin import PluginInterface, Subcommand
from buildscripts.resmokelib.setup_multiversion.setup_multiversion import SetupMultiversion, _DownloadOptions
from buildscripts.resmokelib.utils import evergreen_conn
from buildscripts.resmokelib.utils.filesystem import build_hygienic_bin_path

_HELP = """
Symbolize a backtrace JSON file given an Evergreen Task ID.
"""

LOGGER = None

_MESSAGE = """TODO"""

_COMMAND = "symbolize"

DEFAULT_SYMBOLIZER_LOCATION = "/opt/mongodbtoolchain/v3/bin/llvm-symbolizer"


def setup_logging(debug=False):
    """Enable logging."""
    log_level = logging.DEBUG if debug else logging.INFO
    logging.basicConfig(
        format="[%(asctime)s - %(name)s - %(levelname)s] %(message)s",
        level=log_level,
        stream=sys.stdout,
    )
    logging.getLogger("urllib3").setLevel(logging.WARNING)
    logging.getLogger("github").setLevel(logging.WARNING)
    structlog.configure(logger_factory=structlog.stdlib.LoggerFactory())


class Symbolizer(Subcommand):
    """Interact with Symbolizer."""

    def __init__(self, task_id, execution_num, bin_name, mongosym_fwd_args):
        """Constructor."""
        self.execution_num = execution_num
        self.bin_name = bin_name
        self.mongosym_args = mongosym_fwd_args

        self.evg_api: evergreen_conn.RetryingEvergreenApi = evergreen_conn.get_evergreen_api()
        self.multiversion_setup = self._get_multiversion_setup()
        self.task_info = self.evg_api.task_by_id(task_id)

        self.dest_dir = None  # Populated later.

    @staticmethod
    def _get_multiversion_setup():
        # Add the args we care about.
        download_options = _DownloadOptions(db=True, ds=True, da=False)
        return SetupMultiversion(download_options=download_options, ignore_failed_push=True)

    def _get_compile_artifacts(self):
        version_id = self.task_info.version_id
        buildvariant_name = self.task_info.build_variant

        urls = self.multiversion_setup.get_urls(binary_version=None, evergreen_version=version_id,
                                                buildvariant_name=buildvariant_name)

        self.multiversion_setup.download_and_extract_from_urls(urls, bin_suffix=None,
                                                               install_dir=self.dest_dir)

    def _patch_diff_by_id(self):
        version_id = self.task_info.version_id
        module_diffs = evergreen_conn.get_patch_module_diffs(self.evg_api, version_id)

        # Not a patch build.
        if not module_diffs:
            return

        for module_name, diff in module_diffs.items():
            # TODO: enterprise.
            if "mongodb-mongo-" in module_name:
                with open(os.path.join(self.dest_dir, "patch.diff"), 'w') as git_diff_file:
                    git_diff_file.write(diff)
                    subprocess.run(["git", "apply", "patch.diff"], cwd=self.dest_dir, check=True)

    def _get_source(self):
        revision = self.task_info.revision
        source_url = f"https://github.com/mongodb/mongo/archive/{revision}.zip"
        # TODO: enterprise.

        try:
            # Get source for community. No need for entire repo to use `git apply [patch]`.
            src_parent_dir = os.path.dirname(self.dest_dir)
            try:
                os.makedirs(src_parent_dir)
            except FileExistsError:
                pass

            subprocess.run(["curl", "-L", "-o", "source.zip", source_url], cwd=src_parent_dir,
                           check=True)
            subprocess.run(["unzip", "-q", "source.zip"], cwd=src_parent_dir, check=True)
            subprocess.run(["rm", "source.zip"], cwd=src_parent_dir, check=True)

            # Do a little dance to get the downloaded source into `self.dest_dir`
            src_dir = os.path.join(src_parent_dir, f"mongo-{revision}")
            if not os.path.isdir(src_dir):
                raise FileNotFoundError(
                    f"source file directory {src_dir} not found; please reach out to #server-testing for assistance"
                )
            os.rename(src_dir, self.dest_dir)

        except subprocess.CalledProcessError as err:
            LOGGER.error(err.stdout)
            LOGGER.error(err.stderr)
            raise

    def _setup_symbols(self):
        try:
            self.dest_dir = os.path.join("build", "multiversion", self.task_info.build_id)

            if os.path.isdir(self.dest_dir):
                LOGGER.info(
                    "directory for build already exists, skipping fetching source and symbols")
                return

            LOGGER.info("Getting source from GitHub...")
            self._get_source()
            LOGGER.info("Downloading debug symbols and binaries, this may take a few minutes...")
            self._get_compile_artifacts()
            LOGGER.info("Applying patch diff (if any)...")
            self._patch_diff_by_id()

        except:  # pylint: disable=bare-except
            if self.dest_dir is not None:
                LOGGER.warning("Removing downloaded directory due to error",
                               directory=self.dest_dir)
                shutil.rmtree(self.dest_dir)
            raise

    def _parse_mongosymb_args(self):
        symbolizer_path = self.mongosym_args.symbolizer_path
        if symbolizer_path:
            raise ValueError("Must use the default symbolizer from the toolchain,"
                             f"not {symbolizer_path}")
        self.mongosym_args.symbolizer_path = DEFAULT_SYMBOLIZER_LOCATION

        sym_search_path = self.mongosym_args.path_to_executable
        if sym_search_path:
            raise ValueError(f"Must not specify path_to_executable, the original path that "
                             f"generated the symbols will be used: {sym_search_path}")
        # TODO: support non-hygienic builds.
        self.mongosym_args.path_to_executable = build_hygienic_bin_path(
            parent=self.dest_dir, child=self.bin_name)

        self.mongosym_args.src_dir_to_move = self.dest_dir

    def execute(self) -> None:
        """
        Work your magic.

        :return: None
        """
        self._setup_symbols()
        self._parse_mongosymb_args()
        LOGGER.info("Invoking mongosymb...")
        mongosymb.main(self.mongosym_args)


class SymbolizerPlugin(PluginInterface):
    """Symbolizer for MongoDB stacktraces."""

    def add_subcommand(self, subparsers):
        """
        Add 'symbolize' subcommand.

        :param subparsers: argparse parser to add to
        :return: None
        """
        parser = subparsers.add_parser(_COMMAND, help=_HELP)
        parser.add_argument(
            "--task-id", '-t', action="store", type=str, required=True,
            help="Fetch corresponding binaries and symbols given an Evergreen task ID")
        # TODO: support multiple Evergreen executions.
        parser.add_argument("--execution", "-e", action="store", type=int, default=0,
                            help=argparse.SUPPRESS)
        parser.add_argument(
            "--binary-name", "-b", action="store", type=str, default="mongod",
            help="Base name of the binary that generated the stacktrace; e.g. `mongod` or `mongos`")
        parser.add_argument("--debug", "-d", dest="debug", action="store_true", default=False,
                            help="Set DEBUG logging level.")

        group = parser.add_argument_group(
            "Verbatim mongosymb.py options for advanced usages",
            description="Compatibility not guaranteed, use at your own risk")
        mongosymb.make_argument_parser(group)

    def parse(self, subcommand, parser, parsed_args, **kwargs):
        """
        Return Symbolizer if command is one we recognize.

        :param subcommand: equivalent to parsed_args.command
        :param parser: parser used
        :param parsed_args: output of parsing
        :param kwargs: additional args
        :return: None or a Subcommand
        """

        if subcommand != _COMMAND:
            return None

        setup_logging(parsed_args.debug)
        global LOGGER  # pylint: disable=global-statement
        LOGGER = structlog.getLogger(__name__)

        task_id = parsed_args.task_id
        binary_name = parsed_args.binary_name

        if not task_id:
            raise ValueError(
                "A valid Evergreen Task ID is required. You can get it by double clicking the"
                " Evergreen URL after `/task/` on any task page")

        if not binary_name:
            raise ValueError("A binary base name is required. This is usually `mongod` or `mongos`")

        # Check is Linux.
        if not os.path.isfile(DEFAULT_SYMBOLIZER_LOCATION):
            raise ValueError("llvm-symbolizer in MongoDB toolchain not found. Please run this on a "
                             "virtual workstation or install the toolchain manually")

        if not os.access("/data/mci", os.W_OK):
            raise ValueError("Please ensure you have write access to /data/mci. "
                             "E.g. with `sudo mkdir -p /data/mci; sudo chown $USER /data/mci`")

        return Symbolizer(task_id, parsed_args.execution, binary_name, parsed_args)
