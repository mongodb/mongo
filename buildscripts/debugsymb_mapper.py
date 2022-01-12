"""Script to generate & upload 'buildId -> debug symbols URL' mappings to symbolizer service."""
import argparse
import json
import logging
import os
import pathlib
import shutil
import subprocess
import sys
import time
import typing

import requests
from db_contrib_tool.setup_repro_env.setup_repro_env import SetupReproEnv, download

# register parent directory in sys.path, so 'buildscripts' is detected no matter where the script is called from
sys.path.append(str(pathlib.Path(os.path.join(os.getcwd(), __file__)).parent.parent))

# pylint: disable=wrong-import-position
from buildscripts.util.oauth import get_client_cred_oauth_credentials, Configs


class LinuxBuildIDExtractor:
    """Parse readlef command output & extract Build ID."""

    default_executable_path = "readelf"

    def __init__(self, executable_path: str = None):
        """Initialize instance."""

        self.executable_path = executable_path or self.default_executable_path

    def callreadelf(self, binary_path: str) -> str:
        """Call readelf command for given binary & return string output."""

        args = [self.executable_path, "-n", binary_path]
        process = subprocess.Popen(args=args, close_fds=True, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE)
        process.wait()
        return process.stdout.read().decode()

    @staticmethod
    def extractbuildid(out: str) -> typing.Optional[str]:
        """Parse readelf output and extract Build ID from it."""

        build_id = None
        for line in out.splitlines():
            line = line.strip()
            if line.startswith('Build ID'):
                if build_id is not None:
                    raise ValueError("Found multiple Build ID values.")
                build_id = line.split(': ')[1]
        return build_id

    def run(self, binary_path: str) -> typing.Tuple[str, str]:
        """Perform all necessary actions to get Build ID."""

        readelfout = self.callreadelf(binary_path)
        buildid = self.extractbuildid(readelfout)

        return buildid, readelfout


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

    # pylint: disable=too-many-instance-attributes
    # pylint: disable=too-many-arguments
    # This amount of attributes are necessary.

    default_web_service_base_url: str = "https://symbolizer-service.server-tig.prod.corp.mongodb.com"
    default_cache_dir = os.path.join(os.getcwd(), 'build', 'symbols_cache')
    selected_binaries = ('mongos.debug', 'mongod.debug', 'mongo.debug')
    default_client_credentials_scope = "servertig-symbolizer-fullaccess"
    default_client_credentials_user_name = "client-user"
    default_creds_file_path = os.path.join(os.getcwd(), '.symbolizer_credentials.json')

    def __init__(self, version: str, client_id: str, client_secret: str, variant: str,
                 cache_dir: str = None, web_service_base_url: str = None,
                 logger: logging.Logger = None):
        """
        Initialize instance.

        :param version: version string
        :param variant: build variant string
        :param cache_dir: full path to cache directory as a string
        :param web_service_base_url: URL of symbolizer web service
        """
        self.version = version
        self.variant = variant
        self.cache_dir = cache_dir or self.default_cache_dir
        self.web_service_base_url = web_service_base_url or self.default_web_service_base_url

        if not logger:
            logging.basicConfig()
            logger = logging.getLogger('symbolizer')
            logger.setLevel(logging.INFO)
        self.logger = logger

        self.http_client = requests.Session()

        self.multiversion_setup = SetupReproEnv(
            DownloadOptions(download_symbols=True, download_binaries=True), variant=self.variant,
            ignore_failed_push=True)
        self.debug_symbols_url = None
        self.url = None
        self.configs = Configs(
            client_credentials_scope=self.default_client_credentials_scope,
            client_credentials_user_name=self.default_client_credentials_user_name)
        self.client_id = client_id
        self.client_secret = client_secret

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
                    # credentials hasn't expired yet
                    self.http_client.headers.update({"Authorization": f"Bearer {access_token}"})
                    return

        credentials = get_client_cred_oauth_credentials(self.client_id, self.client_secret,
                                                        configs=self.configs)
        self.http_client.headers.update({"Authorization": f"Bearer {credentials.access_token}"})

        # write credentials to local file for further useage
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

        urls = self.multiversion_setup.get_urls(self.version, self.variant).urls

        download_symbols_url = urls.get("mongo-debugsymbols.tgz", None)
        binaries_url = urls.get("Binaries", "")

        if not download_symbols_url:
            download_symbols_url = urls.get("mongo-debugsymbols.zip", None)

        if not download_symbols_url:
            self.logger.error("Couldn't find URL for debug symbols. Version: %s, URLs dict: %s",
                              self.version, urls)
            raise ValueError(f"Debug symbols URL not found. URLs dict: {urls}")

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

    def generate_build_id_mapping(self) -> typing.Generator[typing.Dict[str, str], None, None]:
        """
        Extract build id from binaries and creates new dict using them.

        :return: mapped data as dict
        """

        readelf_extractor = LinuxBuildIDExtractor()

        debug_symbols_path = self.download(self.debug_symbols_url)
        debug_symbols_unpacked_path = self.unpack(debug_symbols_path)

        binaries_path = self.download(self.url)
        binaries_unpacked_path = self.unpack(binaries_path)

        # we need to analyze two directories: bin inside debug-symbols and lib inside binaries.
        # bin holds main binaries, like mongos, mongod, mongo ...
        # lib holds shared libraries, tons of them. some build variants do not contain shared libraries.

        debug_symbols_unpacked_path = os.path.join(debug_symbols_unpacked_path, 'dist-test')
        binaries_unpacked_path = os.path.join(binaries_unpacked_path, 'dist-test')

        self.logger.info("INSIDE unpacked debug-symbols/dist-test: %s",
                         os.listdir(debug_symbols_unpacked_path))
        self.logger.info("INSIDE unpacked binaries/dist-test: %s",
                         os.listdir(binaries_unpacked_path))

        # start with 'bin' folder
        for binary in self.selected_binaries:
            full_bin_path = os.path.join(debug_symbols_unpacked_path, 'bin', binary)

            if not os.path.exists(full_bin_path):
                self.logger.error("Could not find binary at %s", full_bin_path)
                return

            build_id, readelf_out = readelf_extractor.run(full_bin_path)

            if not build_id:
                self.logger.error("Build ID couldn't be extracted. \nReadELF output %s",
                                  readelf_out)
                return

            yield {
                'url': self.url, 'debug_symbols_url': self.debug_symbols_url, 'build_id': build_id,
                'file_name': binary, 'version': self.version
            }

        # move to 'lib' folder.
        # it contains all shared library binary files,
        # we run readelf on each of them.
        lib_folder_path = os.path.join(binaries_unpacked_path, 'lib')

        if not os.path.exists(lib_folder_path):
            # sometimes we don't get lib folder, which means there is no shared libraries for current build variant.
            self.logger.info("'lib' folder does not exist.")
            sofiles = []
        else:
            sofiles = os.listdir(lib_folder_path)
            self.logger.info("'lib' folder: %s", sofiles)

        for sofile in sofiles:
            sofile_path = os.path.join(lib_folder_path, sofile)

            if not os.path.exists(sofile_path):
                self.logger.error("Could not find binary at %s", sofile_path)
                return

            build_id, readelf_out = readelf_extractor.run(sofile_path)

            if not build_id:
                self.logger.error("Build ID couldn't be extracted. \nReadELF out %s", readelf_out)
                return

            yield {
                'url': self.url,
                'debug_symbols_url': self.debug_symbols_url,
                'build_id': build_id,
                'file_name': sofile,
                'version': self.version,
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

    mapper = Mapper(version=options.version, variant=options.variant, client_id=options.client_id,
                    client_secret=options.client_secret,
                    web_service_base_url=options.web_service_base_url)

    # when used as a context manager, mapper instance automatically cleans files/folders after finishing its job.
    # in other cases, mapper.cleanup() method should be called manually.
    with mapper:
        mapper.run()


if __name__ == '__main__':
    mapper_options = make_argument_parser(description=__doc__).parse_args()
    main(mapper_options)
