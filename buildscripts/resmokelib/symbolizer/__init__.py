"""Wrapper around mongosym to download everything required."""

import logging
import os
import shutil
import subprocess
import sys
from typing import Optional

from buildscripts import mongosymb
from buildscripts.resmokelib.plugin import PluginInterface, Subcommand
from buildscripts.resmokelib.setup_multiversion.setup_multiversion import (
    SetupMultiversion,
    _DownloadOptions,
)
from buildscripts.resmokelib.utils import evergreen_conn
from buildscripts.resmokelib.utils.filesystem import build_hygienic_bin_path, mkdtemp_in_build_dir

_HELP = """
Symbolize a backtrace JSON file given an Evergreen Task ID.
"""

_MESSAGE = """TODO"""

_COMMAND = "symbolize"

DEFAULT_SYMBOLIZER_LOCATION = "/opt/mongodbtoolchain/v4/bin/llvm-symbolizer"


class Symbolizer(Subcommand):
    """Interact with Symbolizer."""

    def __init__(
        self,
        task_id,
        download_symbols_only,
        bin_name=None,
        all_args=None,
        logger: Optional[logging.Logger] = None,
    ):
        """Constructor."""

        self._validate_args(task_id, download_symbols_only, bin_name)
        self.bin_name = bin_name
        self.mongosym_args = all_args
        self.download_symbols_only = download_symbols_only

        self.evg_api: evergreen_conn.RetryingEvergreenApi = evergreen_conn.get_evergreen_api()
        self.multiversion_setup = self._get_multiversion_setup()
        self.task_info = self.evg_api.task_by_id(task_id)

        if download_symbols_only:
            # If only downloading symbols, just extract them into the working directory
            # since this use case is for tasks running in Evergreen.
            self.dest_dir = os.getcwd()
        else:
            self.dest_dir = os.path.join("build", "symbolizer", self.task_info.build_id)

        # Get source for community. No need for entire repo to use `git apply [patch]`.
        src_parent_dir = os.path.dirname(self.dest_dir)
        try:
            os.makedirs(src_parent_dir)
        except FileExistsError:
            pass

        self.logger = logger or self.setup_logger()

    @staticmethod
    def setup_logger(debug=False) -> logging.Logger:
        """
        Setup logger.

        :param debug: Whether to enable debugging or not.
        :return: Logger instance.
        """
        logging.getLogger("urllib3").setLevel(logging.WARNING)
        logging.getLogger("github").setLevel(logging.WARNING)
        log_level = logging.DEBUG if debug else logging.INFO
        logger = logging.Logger("symbolizer", level=log_level)
        handler = logging.StreamHandler(sys.stdout)
        handler.setFormatter(
            logging.Formatter(fmt="[%(asctime)s - %(name)s - %(levelname)s] %(message)s")
        )
        logger.addHandler(handler)
        return logger

    @staticmethod
    def _validate_args(task_id, download_symbols_only, bin_name):
        if not task_id:
            raise ValueError(
                "A valid Evergreen Task ID is required. You can get it by double clicking the"
                " Evergreen URL after `/task/` on any task page"
            )

        if not download_symbols_only:
            if not bin_name:
                raise ValueError(
                    "A binary base name is required. This is usually `mongod` or `mongos`"
                )

            if not os.path.isfile(DEFAULT_SYMBOLIZER_LOCATION):
                raise ValueError(
                    "llvm-symbolizer in MongoDB toolchain not found. Please run this on a "
                    "virtual workstation or install the toolchain manually"
                )

            if not os.access("/data/mci", os.W_OK):
                raise ValueError(
                    "Please ensure you have write access to /data/mci. "
                    "E.g. with `sudo mkdir -p /data/mci; sudo chown $USER /data/mci`"
                )

    def _get_multiversion_setup(self):
        if self.download_symbols_only:
            download_options = _DownloadOptions(db=False, ds=True, da=False, dv=False)
        else:
            download_options = _DownloadOptions(db=True, ds=True, da=False, dv=False)
        return SetupMultiversion(download_options=download_options, ignore_failed_push=True)

    def _get_compile_artifacts(self):
        """Download compile artifacts from Evergreen."""
        version_id = self.task_info.version_id
        buildvariant_name = self.task_info.build_variant

        urlinfo = self.multiversion_setup.get_urls(
            version=version_id, buildvariant_name=buildvariant_name
        )
        self.logger.info("Found urls to download and extract %s", urlinfo.urls)

        self.multiversion_setup.download_and_extract_from_urls(
            urlinfo.urls, bin_suffix=None, install_dir=self.dest_dir
        )

    def _patch_diff_by_id(self):
        version_id = self.task_info.version_id
        module_diffs = evergreen_conn.get_patch_module_diffs(self.evg_api, version_id)

        # Not a patch build.
        if not module_diffs:
            return

        for module_name, diff in module_diffs.items():
            # TODO: enterprise.
            if "mongodb-mongo-" in module_name:
                with open(os.path.join(self.dest_dir, "patch.diff"), "w") as git_diff_file:
                    git_diff_file.write(diff)
                    subprocess.run(["git", "apply", "patch.diff"], cwd=self.dest_dir, check=True)

    def _get_source(self):
        revision = self.task_info.revision
        source_url = f"https://github.com/mongodb/mongo/archive/{revision}.zip"
        # TODO: enterprise.

        try:
            cache_dir = mkdtemp_in_build_dir()
            subprocess.run(
                ["curl", "-L", "-o", "source.zip", source_url], cwd=cache_dir, check=True
            )
            subprocess.run(["unzip", "-q", "source.zip"], cwd=cache_dir, check=True)
            subprocess.run(["rm", "source.zip"], cwd=cache_dir, check=True)

            # Do a little dance to get the downloaded source into `self.dest_dir`
            src_dir = os.path.join(cache_dir, f"mongo-{revision}")
            if not os.path.isdir(src_dir):
                raise FileNotFoundError(
                    f"source file directory {src_dir} not found; please reach out to #server-testing for assistance"
                )
            os.rename(src_dir, self.dest_dir)

        except subprocess.CalledProcessError as err:
            self.logger.error(err.stdout)
            self.logger.error(err.stderr)
            raise

    def _setup_symbols(self):
        try:
            if os.path.isdir(self.dest_dir):
                self.logger.info(
                    "directory for build already exists, skipping fetching source and symbols"
                )
                return

            self.logger.info("Getting source from GitHub...")
            self._get_source()
            self.logger.info(
                "Downloading debug symbols and binaries, this may take a few minutes..."
            )
            self._get_compile_artifacts()
            self.logger.info("Applying patch diff (if any)...")
            self._patch_diff_by_id()

        except:
            if self.dest_dir is not None:
                self.logger.warning(
                    "Removing downloaded directory due to error, directory to be removed: %s",
                    self.dest_dir,
                )
                shutil.rmtree(self.dest_dir)
            raise

    def _parse_mongosymb_args(self):
        symbolizer_path = self.mongosym_args.symbolizer_path
        if symbolizer_path:
            raise ValueError(
                "Must use the default symbolizer from the toolchain," f"not {symbolizer_path}"
            )
        self.mongosym_args.symbolizer_path = DEFAULT_SYMBOLIZER_LOCATION

        sym_search_path = self.mongosym_args.path_to_executable
        if sym_search_path:
            raise ValueError(
                f"Must not specify path_to_executable, the original path that "
                f"generated the symbols will be used: {sym_search_path}"
            )

        # TODO: support non-hygienic builds.
        self.mongosym_args.path_to_executable = build_hygienic_bin_path(
            parent=self.dest_dir, child=self.bin_name
        )

        self.mongosym_args.src_dir_to_move = self.dest_dir

    def execute(self) -> None:
        """
        Work your magic.

        :return: None
        """

        if self.download_symbols_only:
            self._get_compile_artifacts()
        else:
            self._setup_symbols()
            self._parse_mongosymb_args()
            self.logger.info("Invoking mongosymb...")
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
            "--task-id",
            "-t",
            action="store",
            type=str,
            required=True,
            help="Fetch corresponding binaries and/or symbols given an Evergreen task ID",
        )

        parser.add_argument(
            "--binary-name",
            "-b",
            action="store",
            type=str,
            default="mongod",
            help="Base name of the binary that generated the stacktrace; e.g. `mongod` or `mongos`",
        )

        parser.add_argument(
            "--download-symbols-only",
            "-s",
            action="store_true",
            default=False,
            help="Just download the debug symbol tarball for the given task-id",
        )

        parser.add_argument(
            "--debug",
            "-d",
            dest="debug",
            action="store_true",
            default=False,
            help="Set DEBUG logging level.",
        )

        group = parser.add_argument_group(
            "Verbatim mongosymb.py options for advanced usages",
            description="Compatibility not guaranteed, use at your own risk",
        )
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

        task_id = parsed_args.task_id
        binary_name = parsed_args.binary_name
        download_symbols_only = parsed_args.download_symbols_only

        return Symbolizer(
            task_id,
            download_symbols_only=download_symbols_only,
            bin_name=binary_name,
            all_args=parsed_args,
            logger=Symbolizer.setup_logger(parsed_args.debug),
        )
