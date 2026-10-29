"""Script to generate & upload 'buildId -> debug symbols URL' mappings to symbolizer service."""

import argparse
import json
import logging
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time
from json import JSONDecoder
from typing import Generator, NamedTuple, Optional

import requests
from tenacity import (
    Retrying,
    before_sleep_log,
    retry_if_result,
    stop_after_attempt,
    wait_exponential,
)

# register parent directory in sys.path, so 'buildscripts' is detected no matter where the script is called from
sys.path.append(str(pathlib.Path(os.path.join(os.getcwd(), __file__)).parent.parent))

from buildscripts.build_system_options import PathOptions
from buildscripts.resmokelib.setup_multiversion.setup_multiversion import (
    SetupMultiversion,
    download,
)
from buildscripts.resmokelib.utils import evergreen_conn
from buildscripts.util.oauth import Configs, get_client_cred_oauth_credentials

BUILD_INFO_RE = re.compile(r"Build Info: ({(\n.*)*})")
MONGOD = "mongod"
DEBUG_INFO_SUFFIXES = (".debug", ".dwp", ".pdb")

# Display-name prefix of the "archive failed tests" s3.put that uploads the relinked test
# binaries from the resmoke_tests task.
FAILED_TESTS_ARTIFACT_PREFIX = "Test binaries and libraries"


class CmdClient:
    """Client to run commands."""

    @staticmethod
    def run(args: list[str]) -> str:
        """
        Run command with args.

        :param args: Argument list.
        :return: Command output.
        """

        out = subprocess.run(
            args, close_fds=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False
        )
        return out.stdout.strip().decode()


class BuildIdOutput(NamedTuple):
    """
    Build ID and command output.

    * build_id: Build ID or None.
    * cmd_output: Command output.
    """

    build_id: Optional[str]
    cmd_output: str


class BinVersionOutput(NamedTuple):
    """
    Mongodb bin version and command output.

    * mongodb_version: Bin version.
    * cmd_output: Command output.
    """

    mongodb_version: Optional[str]
    cmd_output: str


class CmdOutputExtractor:
    """Data extractor from command output."""

    def __init__(
        self, cmd_client: Optional[CmdClient] = None, json_decoder: Optional[JSONDecoder] = None
    ) -> None:
        """
        Initialize.

        :param cmd_client: Client to run commands.
        :param json_decoder: JSONDecoder object.
        """
        self.cmd_client = cmd_client if cmd_client is not None else CmdClient()
        self.json_decoder = json_decoder if json_decoder is not None else JSONDecoder()

    def get_build_id(self, bin_path: str) -> BuildIdOutput:
        """
        Get build ID from readelf command.

        :param bin_path: Path to binary of the build.
        :return: Build ID or None and command output.
        """
        out = self.cmd_client.run(["readelf", "-n", bin_path])
        build_id = self._extract_build_id(out)
        return BuildIdOutput(build_id, out)

    def get_bin_version(self, bin_path: str) -> BinVersionOutput:
        """
        Get mongodb bin version from `{bin} --version` command.

        :param bin_path: Path to mongodb binary.
        :return: Bin version or None and command output.
        """
        out = self.cmd_client.run([os.path.abspath(bin_path), "--version"])
        mongodb_version = self._get_mongodb_version(out)
        return BinVersionOutput(mongodb_version, out)

    @staticmethod
    def _extract_build_id(out: str) -> Optional[str]:
        """
        Parse readelf output and extract Build ID from it.

        :param out: readelf command output.
        :return: Build ID on None.
        """
        build_id = None
        for line in out.splitlines():
            line = line.strip()
            if line.startswith("Build ID"):
                if build_id is not None:
                    raise ValueError("Found multiple Build ID values.")
                build_id = line.split(": ")[1]
        return build_id

    def _get_mongodb_version(self, out: str) -> Optional[str]:
        """
        Parse version command output and extract mongodb version.

        :param out: Version command output.
        :return: Version or None.
        """
        mongodb_version = None

        search = BUILD_INFO_RE.search(out)
        if search:
            build_info = self.json_decoder.decode(search.group(1))
            mongodb_version = build_info.get("version")

        return mongodb_version


class DownloadOptions(object):
    """A class to collect download option configurations."""

    def __init__(
        self,
        download_binaries=False,
        download_symbols=False,
        download_artifacts=False,
        download_python_venv=False,
    ):
        """Initialize instance."""

        self.download_binaries = download_binaries
        self.download_symbols = download_symbols
        self.download_artifacts = download_artifacts
        self.download_python_venv = download_python_venv


class Mapper:
    """A class to to basically all of the work."""

    # This amount of attributes are necessary.

    default_web_service_base_url: str = (
        "https://symbolizer-service.server-tig.prod.corp.mongodb.com"
    )
    default_cache_dir = os.path.join(os.getcwd(), "build", "symbols_cache")
    selected_binaries = ("mongos", "mongod", "mongo")
    default_client_credentials_scope = "servertig-symbolizer-fullaccess"
    default_client_credentials_user_name = "client-user"
    default_creds_file_path = os.path.join(os.getcwd(), ".symbolizer_credentials.json")

    # The Evergreen API serves artifact lists from a secondary node
    # (DEVPROD-32223), so artifacts attached by a recently-finished compile
    # task can be missing from the response due to replication lag. Retry the
    # lookup with backoff before giving up.
    num_url_retries = 8
    url_retry_initial_delay_secs = 15
    url_retry_max_delay_secs = 120

    def __init__(
        self,
        evg_version: str,
        evg_variant: str,
        is_san_variant: bool,
        client_id: str,
        client_secret: str,
        cache_dir: str = None,
        web_service_base_url: str = None,
        logger: logging.Logger = None,
        binaries_dir: str = None,
        binaries_url: str = None,
        debug_symbols_url: str = None,
        mongodb_version: str = None,
        task_id: str = None,
    ):
        """
        Initialize instance.

        :param evg_version: Evergreen version ID.
        :param evg_variant: Evergreen build variant name.
        :param is_san_variant: Whether build variant is sanitizer build.
        :param client_id: Client id for Okta Oauth.
        :param client_secret: Secret key for Okta Oauth.
        :param cache_dir: Full path to cache directory as a string.
        :param web_service_base_url: URL of symbolizer web service.
        :param logger: Debug symbols mapper logger.
        :param binaries_dir: Local directory of already-built binaries to map.
        :param binaries_url: Download URL to record for the binaries in `binaries_dir`.
        :param debug_symbols_url: Download URL to record for their debug symbols.
        :param mongodb_version: Server version to record when it cannot be read off a binary.
        :param task_id: Evergreen task whose "Test binaries and libraries" artifact holds the
            binaries in `binaries_dir`.
        """
        self.evg_version = evg_version
        self.evg_variant = evg_variant
        self.is_san_variant = is_san_variant
        self.cache_dir = cache_dir or self.default_cache_dir
        self.web_service_base_url = web_service_base_url or self.default_web_service_base_url
        self.binaries_dir = binaries_dir
        self.mongodb_version = mongodb_version
        self.task_id = task_id

        if not logger:
            logging.basicConfig()
            logger = logging.getLogger("symbolizer")
            logger.setLevel(logging.INFO)
        self.logger = logger

        self.http_client = requests.Session()

        self.debug_symbols_url = None
        self.url = None
        self.configs = Configs(
            client_credentials_scope=self.default_client_credentials_scope,
            client_credentials_user_name=self.default_client_credentials_user_name,
        )
        self.client_id = client_id
        self.client_secret = client_secret
        self.path_options = PathOptions()
        self.extractor = CmdOutputExtractor()

        if not os.path.exists(self.cache_dir):
            os.makedirs(self.cache_dir)

        self.authenticate()

        if self.binaries_dir:
            self.multiversion_setup = None
            if binaries_url:
                self.url = binaries_url
                self.debug_symbols_url = debug_symbols_url or binaries_url
            elif task_id:
                self.url = self._resolve_failed_tests_artifact_url()
                self.debug_symbols_url = debug_symbols_url or self.url
            else:
                raise ValueError(
                    "--binaries-url is required when --binaries-dir is given (or pass --task-id "
                    "to resolve the uploaded archive's URL from Evergreen)."
                )
        else:
            self.multiversion_setup = SetupMultiversion(
                DownloadOptions(download_symbols=True, download_binaries=True),
                variant=self.evg_variant,
                ignore_failed_push=True,
            )
            self.setup_urls()

    def _resolve_failed_tests_artifact_url(self) -> str:
        """
        Resolve the download URL of this task's failed-tests archive from the Evergreen API.

        :return: The artifact's URL.
        """
        evg_api = evergreen_conn.get_evergreen_api()

        def get_artifact_urls() -> dict[str, str]:
            task = evg_api.task_by_id(self.task_id)
            return {
                artifact.name: artifact.url
                for artifact in task.artifacts
                if artifact.name.startswith(FAILED_TESTS_ARTIFACT_PREFIX)
            }

        retrying = Retrying(
            retry=retry_if_result(lambda artifact_urls: not artifact_urls),
            wait=wait_exponential(
                multiplier=self.url_retry_initial_delay_secs, max=self.url_retry_max_delay_secs
            ),
            stop=stop_after_attempt(self.num_url_retries),
            before_sleep=before_sleep_log(self.logger, logging.WARNING),
            retry_error_callback=lambda retry_state: retry_state.outcome.result(),
        )
        artifact_urls = retrying(get_artifact_urls)

        if not artifact_urls:
            self.logger.error("Couldn't find the failed-tests artifact for task %s.", self.task_id)
            raise ValueError(f"Failed-tests artifact not found for task {self.task_id}")

        return next(iter(artifact_urls.values()))

    def authenticate(self):
        """Login & get credentials for further requests to web service."""

        # try to read from file
        if os.path.exists(self.default_creds_file_path):
            with open(self.default_creds_file_path) as cfile:
                data = json.loads(cfile.read())
                access_token, expire_time = data.get("access_token"), data.get("expire_time")
                if time.time() < expire_time:
                    # credentials haven't expired yet
                    self.http_client.headers.update({"Authorization": f"Bearer {access_token}"})
                    return

        credentials = get_client_cred_oauth_credentials(
            self.client_id, self.client_secret, configs=self.configs
        )
        self.http_client.headers.update({"Authorization": f"Bearer {credentials.access_token}"})

        # write credentials to local file for further usage
        with open(self.default_creds_file_path, "w") as cfile:
            cfile.write(
                json.dumps(
                    {
                        "access_token": credentials.access_token,
                        "expire_time": time.time() + credentials.expires_in,
                    }
                )
            )

    def __enter__(self):
        """Return instance when used as a context manager."""

        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        """Do cleaning process when used as a context manager."""

        self.cleanup()

    def cleanup(self):
        """Remove temporary files & folders."""

        if os.path.exists(self.cache_dir):
            shutil.rmtree(self.cache_dir)

    @staticmethod
    def url_to_filename(url: str) -> str:
        """
        Convert URL to local filename.

        :param url: download URL
        :return: full name for local file
        """
        return url.split("/")[-1]

    def setup_urls(self):
        """Set up URLs using multiversion."""

        def get_urls() -> tuple[str, str, dict]:
            urlinfo = self.multiversion_setup.get_urls(self.evg_version, self.evg_variant)

            binaries_url = urlinfo.urls.get("Binaries", "")
            if self.is_san_variant:
                # Sanitizer builds are not stripped and contain debug symbols
                download_symbols_url = binaries_url
            else:
                download_symbols_url = urlinfo.urls.get(
                    "mongo-debugsymbols.tgz"
                ) or urlinfo.urls.get("mongo-debugsymbols.zip")

            return binaries_url, download_symbols_url, urlinfo.urls

        retrying = Retrying(
            retry=retry_if_result(lambda result: not result[0] or not result[1]),
            wait=wait_exponential(
                multiplier=self.url_retry_initial_delay_secs, max=self.url_retry_max_delay_secs
            ),
            stop=stop_after_attempt(self.num_url_retries),
            before_sleep=before_sleep_log(self.logger, logging.WARNING),
            retry_error_callback=lambda retry_state: retry_state.outcome.result(),
        )
        binaries_url, download_symbols_url, urls = retrying(get_urls)

        if not binaries_url or not download_symbols_url:
            self.logger.error(
                "Couldn't find URL for debug symbols. Version: %s, URLs dict: %s",
                self.evg_version,
                urls,
            )
            raise ValueError(f"Debug symbols URL not found. URLs dict: {urls}")

        self.debug_symbols_url = download_symbols_url
        self.url = binaries_url

    def unpack(self, path: str) -> str:
        """
        Use to untar/unzip files.

        :param path: full path of file
        :return: full path of directory of unpacked file
        """
        foldername = path.replace(".tgz", "", 1).split("/")[-1]
        out_dir = os.path.join(self.cache_dir, foldername)

        if not os.path.exists(out_dir):
            os.makedirs(out_dir)

        download.extract_archive(path, out_dir)

        # extracted everything, we don't need the original tar file anymore and it should be deleted
        if os.path.exists(path):
            os.remove(path)

        return out_dir

    @staticmethod
    def download(url: str) -> str:
        """
        Use to download file from URL.

        :param url: URL of file to download
        :return: full path of downloaded file in local filesystem
        """

        tarball_full_path = download.download_from_s3(url)
        return tarball_full_path

    def _mapping_for(
        self, bin_path: str, file_name: str, mongodb_version: str
    ) -> Optional[dict[str, str]]:
        """
        :param bin_path: Path to the binary to read the build ID from.
        :param file_name: Name to record for the binary, as it appears in a stacktrace.
        :param mongodb_version: Server version to record.
        :return: Mapping as dict, or None when the build ID could not be extracted.
        """
        build_id_output = self.extractor.get_build_id(bin_path)

        if not build_id_output.build_id:
            self.logger.error(
                "Build ID couldn't be extracted from %s. \nReadELF output %s",
                bin_path,
                build_id_output.cmd_output,
            )
            return None

        self.logger.info("Extracted build ID: %s", build_id_output.build_id)

        return {
            "url": self.url,
            "debug_symbols_url": self.debug_symbols_url,
            "build_id": build_id_output.build_id,
            "file_name": file_name,
            "version": mongodb_version,
        }

    def generate_local_build_id_mapping(self) -> Generator[dict[str, str], None, None]:
        """
        Extract build ids from an already-built local tree of binaries.

        :return: mapped data as dict
        """
        bin_dir = os.path.join(self.binaries_dir, self.path_options.main_binary_folder_name)
        lib_dir = os.path.join(self.binaries_dir, self.path_options.shared_library_folder_name)

        mongodb_version = self._resolve_local_mongodb_version(bin_dir)
        if mongodb_version is None:
            return

        mapped_any = False

        for directory in (bin_dir, lib_dir):
            if not os.path.isdir(directory):
                self.logger.info("'%s' does not exist, skipping it.", directory)
                continue

            for file_name in sorted(os.listdir(directory)):
                full_path = os.path.join(directory, file_name)

                if not os.path.isfile(full_path):
                    continue

                if file_name.endswith(DEBUG_INFO_SUFFIXES):
                    continue

                mapping = self._mapping_for(full_path, file_name, mongodb_version)
                if mapping:
                    mapped_any = True
                    yield mapping

        if not mapped_any:
            self.logger.error("Found no mappable binaries under '%s'.", self.binaries_dir)

    def _resolve_local_mongodb_version(self, bin_dir: str) -> Optional[str]:
        """
        Determine the server version to record for a local tree of binaries.

        :param bin_dir: Main binary folder of the local tree.
        :return: Version or None.
        """
        mongod_bin = os.path.join(bin_dir, MONGOD)

        if os.path.exists(mongod_bin):
            bin_version_output = self.extractor.get_bin_version(mongod_bin)
            if bin_version_output.mongodb_version:
                return bin_version_output.mongodb_version
            self.logger.warning(
                "mongodb version could not be extracted. \n`%s --version` output: %s",
                mongod_bin,
                bin_version_output.cmd_output,
            )

        # A failed suite's relink only produces the binaries that suite needed, so mongod is
        # not necessarily present. Fall back to the version the build was stamped with.
        if self.mongodb_version:
            self.logger.info("Using the mongodb version passed in: %s", self.mongodb_version)
            return self.mongodb_version

        self.logger.error(
            "Could not determine the mongodb version: '%s' is absent or unreadable and no "
            "version was passed in with --mongodb-version.",
            mongod_bin,
        )
        return None

    def generate_build_id_mapping(self) -> Generator[dict[str, str], None, None]:
        """
        Extract build id from binaries and creates new dict using them.

        :return: mapped data as dict
        """

        binaries_path = self.download(self.url)
        binaries_unpacked_path = self.unpack(binaries_path)

        # we need to analyze two directories: main binary folder and
        # shared libraries folder inside binaries.
        # main binary folder holds main binaries, like mongos, mongod, mongo ...
        # shared libraries folder holds shared libraries, tons of them.
        # some build variants do not contain shared libraries.

        binaries_unpacked_path = os.path.join(binaries_unpacked_path, "dist-test")

        self.logger.info(
            "INSIDE unpacked binaries/dist-test: %s", os.listdir(binaries_unpacked_path)
        )

        mongod_bin = os.path.join(
            binaries_unpacked_path, self.path_options.main_binary_folder_name, MONGOD
        )
        bin_version_output = self.extractor.get_bin_version(mongod_bin)

        if bin_version_output.mongodb_version is None:
            self.logger.error(
                "mongodb version could not be extracted. \n`%s --version` output: %s",
                mongod_bin,
                bin_version_output.cmd_output,
            )
            return
        else:
            self.logger.info("Extracted mongodb version: %s", bin_version_output.mongodb_version)

        # start with main binary folder
        for binary in self.selected_binaries:
            full_bin_path = os.path.join(
                binaries_unpacked_path, self.path_options.main_binary_folder_name, binary
            )

            if not os.path.exists(full_bin_path):
                self.logger.error("Could not find binary at %s", full_bin_path)
                continue

            mapping = self._mapping_for(full_bin_path, binary, bin_version_output.mongodb_version)
            if mapping:
                yield mapping

        # move to shared libraries folder.
        # it contains all shared library binary files,
        # we run readelf on each of them.
        lib_folder_path = os.path.join(
            binaries_unpacked_path, self.path_options.shared_library_folder_name
        )

        if not os.path.exists(lib_folder_path):
            # sometimes we don't get lib folder, which means there is no shared libraries for current build variant.
            self.logger.info(
                "'%s' folder does not exist.", self.path_options.shared_library_folder_name
            )
            sofiles = []
        else:
            sofiles = os.listdir(lib_folder_path)
            self.logger.info(
                "'%s' folder: %s", self.path_options.shared_library_folder_name, sofiles
            )

        for sofile in sofiles:
            sofile_path = os.path.join(lib_folder_path, sofile)

            if not os.path.exists(sofile_path):
                self.logger.error("Could not find binary at %s", sofile_path)
                continue

            mapping = self._mapping_for(sofile_path, sofile, bin_version_output.mongodb_version)
            if mapping:
                yield mapping

    def run(self):
        """Run all necessary processes."""

        mappings = (
            self.generate_local_build_id_mapping()
            if self.binaries_dir
            else self.generate_build_id_mapping()
        )
        if not mappings:
            self.logger.error("Could not generate mapping")
            return

        # mappings is a generator, we iterate over to generate mappings on the go
        for mapping in mappings:
            self.logger.info("Creating mapping %s", mapping)
            response = self.http_client.post(
                "/".join((self.web_service_base_url, "add")), json=mapping
            )
            if response.status_code != 200:
                self.logger.error(
                    "Could not store mapping, web service returned status code %s from URL %s. "
                    "Response: %s",
                    response.status_code,
                    response.url,
                    response.text,
                )


def make_argument_parser(parser=None, **kwargs):
    """Make and return an argparse."""

    if parser is None:
        parser = argparse.ArgumentParser(**kwargs)

    parser.add_argument("--version")
    parser.add_argument("--client-id")
    parser.add_argument("--client-secret")
    parser.add_argument("--variant")
    parser.add_argument("--is-san-variant", action="store_true")
    parser.add_argument("--web-service-base-url", default="")
    parser.add_argument(
        "--binaries-dir",
        help="Map an already-built local tree of binaries (with 'bin' and 'lib' subfolders) "
        "instead of discovering and downloading compile task artifacts. Requires "
        "--binaries-url or --task-id.",
    )
    parser.add_argument(
        "--binaries-url",
        help="Download URL to record for the binaries in --binaries-dir. Optional when "
        "--task-id is given, which resolves the URL from the task's artifacts in Evergreen.",
    )
    parser.add_argument(
        "--task-id",
        help="Evergreen task whose 'Test binaries and libraries' artifact holds the binaries "
        "in --binaries-dir. Used to resolve --binaries-url when it is not passed.",
    )
    parser.add_argument(
        "--debug-symbols-url",
        help="Download URL to record for the debug symbols of the binaries in --binaries-dir. "
        "Defaults to --binaries-url, which is correct when one archive holds both.",
    )
    parser.add_argument(
        "--mongodb-version",
        help="Server version to record when it cannot be read off one of the binaries.",
    )
    return parser


def main(options):
    """Execute mapper here. Main entry point."""

    mapper = Mapper(
        evg_version=options.version,
        evg_variant=options.variant,
        is_san_variant=options.is_san_variant,
        client_id=options.client_id,
        client_secret=options.client_secret,
        web_service_base_url=options.web_service_base_url,
        binaries_dir=options.binaries_dir,
        binaries_url=options.binaries_url,
        debug_symbols_url=options.debug_symbols_url,
        mongodb_version=options.mongodb_version,
        task_id=options.task_id,
    )

    # when used as a context manager, mapper instance automatically cleans files/folders after finishing its job.
    # in other cases, mapper.cleanup() method should be called manually.
    with mapper:
        mapper.run()


if __name__ == "__main__":
    mapper_options = make_argument_parser(description=__doc__).parse_args()
    main(mapper_options)
