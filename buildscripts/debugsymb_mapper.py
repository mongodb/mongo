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
from typing import Optional, Tuple, Generator, Dict, List, NamedTuple

import requests

# register parent directory in sys.path, so 'buildscripts' is detected no matter where the script is called from
sys.path.append(str(pathlib.Path(os.path.join(os.getcwd(), __file__)).parent.parent))

# pylint: disable=wrong-import-position
from buildscripts.util.oauth import get_client_cred_oauth_credentials, Configs
from buildscripts.resmokelib.setup_multiversion.setup_multiversion import SetupMultiversion, download
from buildscripts.build_system_options import PathOptions

BUILD_INFO_RE = re.compile(r"Build Info: ({(\n.*)*})")
MONGOD = "mongod"


class CmdClient:
    """Client to run commands."""

    @staticmethod
    def run(args: List[str]) -> str:
        """
        Run command with args.

        :param args: Argument list.
        :return: Command output.
        """

        out = subprocess.run(args, close_fds=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             check=False)
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

    def __init__(self, cmd_client: Optional[CmdClient] = None,
                 json_decoder: Optional[JSONDecoder] = None) -> None:
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
            if line.startswith('Build ID'):
                if build_id is not None:
                    raise ValueError("Found multiple Build ID values.")
                build_id = line.split(': ')[1]
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

    def __init__(self, download_binaries=False, download_symbols=False, download_artifacts=False,
                 download_python_venv=False):
        """Initialize instance."""

        self.download_binaries = download_binaries
        self.download_symbols = download_symbols
        self.download_artifacts = download_artifacts
        self.download_python_venv = download_python_venv


class Mapper:
    """A class to to basically all of the work."""

    # This amount of attributes are necessary.

    default_web_service_base_url: str = "https://symbolizer-service.server-tig.prod.corp.mongodb.com"
    default_cache_dir = os.path.join(os.getcwd(), 'build', 'symbols_cache')
    selected_binaries = ('mongos.debug', 'mongod.debug', 'mongo.debug')
    default_client_credentials_scope = "servertig-symbolizer-fullaccess"
    default_client_credentials_user_name = "client-user"
    default_creds_file_path = os.path.join(os.getcwd(), '.symbolizer_credentials.json')

    def __init__(self, evg_version: str, evg_variant: str, client_id: str, client_secret: str,
                 cache_dir: str = None, web_service_base_url: str = None,
                 logger: logging.Logger = None):
        """
        Initialize instance.

        :param evg_version: Evergreen version ID.
        :param evg_variant: Evergreen build variant name.
        :param client_id: Client id for Okta Oauth.
        :param client_secret: Secret key for Okta Oauth.
        :param cache_dir: Full path to cache directory as a string.
        :param web_service_base_url: URL of symbolizer web service.
        :param logger: Debug symbols mapper logger.
        """
        self.evg_version = evg_version
        self.evg_variant = evg_variant
        self.cache_dir = cache_dir or self.default_cache_dir
        self.web_service_base_url = web_service_base_url or self.default_web_service_base_url

        if not logger:
            logging.basicConfig()
            logger = logging.getLogger('symbolizer')
            logger.setLevel(logging.INFO)
        self.logger = logger

        self.http_client = requests.Session()

        self.multiversion_setup = SetupMultiversion(
            DownloadOptions(download_symbols=True, download_binaries=True),
            variant=self.evg_variant, ignore_failed_push=True)
        self.debug_symbols_url = None
        self.url = None
        self.configs = Configs(
            client_credentials_scope=self.default_client_credentials_scope,
            client_credentials_user_name=self.default_client_credentials_user_name)
        self.client_id = client_id
        self.client_secret = client_secret
        self.path_options = PathOptions()

        if not os.path.exists(self.cache_dir):
            os.makedirs(self.cache_dir)

        self.authenticate()
        self.setup_urls()

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

        credentials = get_client_cred_oauth_credentials(self.client_id, self.client_secret,
                                                        configs=self.configs)
        self.http_client.headers.update({"Authorization": f"Bearer {credentials.access_token}"})

        # write credentials to local file for further usage
        with open(self.default_creds_file_path, "w") as cfile:
            cfile.write(
                json.dumps({
                    "access_token": credentials.access_token,
                    "expire_time": time.time() + credentials.expires_in
                }))

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
        return url.split('/')[-1]

    def setup_urls(self):
        """Set up URLs using multiversion."""

        urlinfo = self.multiversion_setup.get_urls(self.evg_version, self.evg_variant)

        download_symbols_url = urlinfo.urls.get("mongo-debugsymbols.tgz", None)
        binaries_url = urlinfo.urls.get("Binaries", "")

        if not download_symbols_url:
            download_symbols_url = urlinfo.urls.get("mongo-debugsymbols.zip", None)

        if not download_symbols_url:
            self.logger.error("Couldn't find URL for debug symbols. Version: %s, URLs dict: %s",
                              self.evg_version, urlinfo.urls)
            raise ValueError(f"Debug symbols URL not found. URLs dict: {urlinfo.urls}")

        self.debug_symbols_url = download_symbols_url
        self.url = binaries_url

    def unpack(self, path: str) -> str:
        """
        Use to untar/unzip files.

        :param path: full path of file
        :return: full path of directory of unpacked file
        """
        foldername = path.replace('.tgz', '', 1).split('/')[-1]
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

    def generate_build_id_mapping(self) -> Generator[Dict[str, str], None, None]:
        """
        Extract build id from binaries and creates new dict using them.

        :return: mapped data as dict
        """

        extractor = CmdOutputExtractor()

        debug_symbols_path = self.download(self.debug_symbols_url)
        debug_symbols_unpacked_path = self.unpack(debug_symbols_path)

        binaries_path = self.download(self.url)
        binaries_unpacked_path = self.unpack(binaries_path)

        # we need to analyze two directories: main binary folder inside debug-symbols and
        # shared libraries folder inside binaries.
        # main binary folder holds main binaries, like mongos, mongod, mongo ...
        # shared libraries folder holds shared libraries, tons of them.
        # some build variants do not contain shared libraries.

        debug_symbols_unpacked_path = os.path.join(debug_symbols_unpacked_path, 'dist-test')
        binaries_unpacked_path = os.path.join(binaries_unpacked_path, 'dist-test')

        self.logger.info("INSIDE unpacked debug-symbols/dist-test: %s",
                         os.listdir(debug_symbols_unpacked_path))
        self.logger.info("INSIDE unpacked binaries/dist-test: %s",
                         os.listdir(binaries_unpacked_path))

        mongod_bin = os.path.join(binaries_unpacked_path, self.path_options.main_binary_folder_name,
                                  MONGOD)
        bin_version_output = extractor.get_bin_version(mongod_bin)

        if bin_version_output.mongodb_version is None:
            self.logger.error("mongodb version could not be extracted. \n`%s --version` output: %s",
                              mongod_bin, bin_version_output.cmd_output)
            return
        else:
            self.logger.info("Extracted mongodb version: %s", bin_version_output.mongodb_version)

        # start with main binary folder
        for binary in self.selected_binaries:
            full_bin_path = os.path.join(debug_symbols_unpacked_path,
                                         self.path_options.main_binary_folder_name, binary)

            if not os.path.exists(full_bin_path):
                self.logger.error("Could not find binary at %s", full_bin_path)
                return

            build_id_output = extractor.get_build_id(full_bin_path)

            if not build_id_output.build_id:
                self.logger.error("Build ID couldn't be extracted. \nReadELF output %s",
                                  build_id_output.cmd_output)
                return
            else:
                self.logger.info("Extracted build ID: %s", build_id_output.build_id)

            yield {
                'url': self.url,
                'debug_symbols_url': self.debug_symbols_url,
                'build_id': build_id_output.build_id,
                'file_name': binary,
                'version': bin_version_output.mongodb_version,
            }

        # move to shared libraries folder.
        # it contains all shared library binary files,
        # we run readelf on each of them.
        lib_folder_path = os.path.join(binaries_unpacked_path,
                                       self.path_options.shared_library_folder_name)

        if not os.path.exists(lib_folder_path):
            # sometimes we don't get lib folder, which means there is no shared libraries for current build variant.
            self.logger.info("'%s' folder does not exist.",
                             self.path_options.shared_library_folder_name)
            sofiles = []
        else:
            sofiles = os.listdir(lib_folder_path)
            self.logger.info("'%s' folder: %s", self.path_options.shared_library_folder_name,
                             sofiles)

        for sofile in sofiles:
            sofile_path = os.path.join(lib_folder_path, sofile)

            if not os.path.exists(sofile_path):
                self.logger.error("Could not find binary at %s", sofile_path)
                return

            build_id_output = extractor.get_build_id(sofile_path)

            if not build_id_output.build_id:
                self.logger.error("Build ID couldn't be extracted. \nReadELF out %s",
                                  build_id_output.cmd_output)
                return
            else:
                self.logger.info("Extracted build ID: %s", build_id_output.build_id)

            yield {
                'url': self.url,
                'debug_symbols_url': self.debug_symbols_url,
                'build_id': build_id_output.build_id,
                'file_name': sofile,
                'version': bin_version_output.mongodb_version,
            }

    def run(self):
        """Run all necessary processes."""

        mappings = self.generate_build_id_mapping()
        if not mappings:
            self.logger.error("Could not generate mapping")
            return

        # mappings is a generator, we iterate over to generate mappings on the go
        for mapping in mappings:
            response = self.http_client.post('/'.join((self.web_service_base_url, 'add')),
                                             json=mapping)
            if response.status_code != 200:
                self.logger.error(
                    "Could not store mapping, web service returned status code %s from URL %s. "
                    "Response: %s", response.status_code, response.url, response.text)


def make_argument_parser(parser=None, **kwargs):
    """Make and return an argparse."""

    if parser is None:
        parser = argparse.ArgumentParser(**kwargs)

    parser.add_argument('--version')
    parser.add_argument('--client-id')
    parser.add_argument('--client-secret')
    parser.add_argument('--variant')
    parser.add_argument('--web-service-base-url', default="")
    return parser


def main(options):
    """Execute mapper here. Main entry point."""

    mapper = Mapper(evg_version=options.version, evg_variant=options.variant,
                    client_id=options.client_id, client_secret=options.client_secret,
                    web_service_base_url=options.web_service_base_url)

    # when used as a context manager, mapper instance automatically cleans files/folders after finishing its job.
    # in other cases, mapper.cleanup() method should be called manually.
    with mapper:
        mapper.run()


if __name__ == '__main__':
    mapper_options = make_argument_parser(description=__doc__).parse_args()
    main(mapper_options)
