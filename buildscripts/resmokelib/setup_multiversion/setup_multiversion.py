"""Setup multiversion mongodb.

Downloads and installs particular mongodb versions (each binary is renamed
to include its version) into an install directory and symlinks the binaries
with versions to another directory. This script supports community and
enterprise builds.
"""
import logging
import os
import sys
import tempfile

import boto3
import structlog
from botocore import UNSIGNED
from botocore.config import Config
from botocore.exceptions import ClientError
from evergreen import RetryingEvergreenApi

from buildscripts.resmokelib.plugin import PluginInterface, Subcommand

SUBCOMMAND = "setup-multiversion"
EVERGREEN_CONFIG_LOCATIONS = (
    # Common for machines in Evergreen
    os.path.join(os.getcwd(), ".evergreen.yml"),
    # Common for local machines
    os.path.expanduser(os.path.join("~", ".evergreen.yml")),
)
S3_BUCKET = "mciuploads"

LOGGER = structlog.getLogger(__name__)


def setup_logging():
    """Enable INFO level logging."""
    logging.basicConfig(
        format="[%(asctime)s - %(name)s - %(levelname)s] %(message)s",
        level=logging.INFO,
        stream=sys.stdout,
    )
    structlog.configure(logger_factory=structlog.stdlib.LoggerFactory())


def get_evergreen_api(evergreen_config):
    """Return evergreen API."""
    config_to_pass = evergreen_config
    if not config_to_pass:
        # Pickup the first config file found in common locations.
        for file in EVERGREEN_CONFIG_LOCATIONS:
            if os.path.isfile(file):
                config_to_pass = file
                break
    try:
        evg_api = RetryingEvergreenApi.get_api(config_file=config_to_pass)
    except Exception as ex:
        LOGGER.error("Most likely something is wrong with evergreen config file.",
                     config_file=config_to_pass)
        raise ex
    else:
        return evg_api


def download_mongodb(url):
    """Download file from S3 bucket by a given URL."""

    LOGGER.info("Downloading mongodb.", url=url)
    s3_key = url.split('/', 3)[-1].replace(f"{S3_BUCKET}/", "")
    filename = os.path.join(tempfile.gettempdir(), url.split('/')[-1])

    LOGGER.info("Downloading mongodb from S3.", s3_bucket=S3_BUCKET, s3_key=s3_key,
                filename=filename)
    s3_client = boto3.client("s3", config=Config(signature_version=UNSIGNED))
    try:
        s3_client.download_file(S3_BUCKET, s3_key, filename)
    except ClientError as s3_client_error:
        LOGGER.error("Download failed due to S3 client error.")
        raise s3_client_error
    except Exception as ex:  # pylint: disable=broad-except
        LOGGER.error("Download failed.")
        raise ex
    else:
        LOGGER.info("Download completed.", filename=filename)

    return filename


class SetupMultiversion(Subcommand):
    """Main class for the hang analyzer subcommand."""

    # pylint: disable=too-many-instance-attributes
    def __init__(self, options):
        """Initialize."""
        self.install_dir = options.install_dir
        self.link_dir = options.link_dir
        self.edition = options.edition.lower()
        self.platform = options.platform.lower()
        self.architecture = options.architecture.lower()
        self.use_latest = options.use_latest
        self.versions = options.versions
        self.evg_api = get_evergreen_api(options.evergreen_config)
        self.git_token = options.git_token

    def execute(self):
        """Execute setup multiversion mongodb."""
        pass  # Not implemented yet.


class SetupMultiversionPlugin(PluginInterface):
    """Integration point for setup-multiversion-mongodb."""

    def parse(self, subcommand, parser, parsed_args, **kwargs):
        """Parse command-line options."""
        setup_logging()

        if subcommand == SUBCOMMAND:
            return SetupMultiversion(parsed_args)

        return None

    def add_subcommand(self, subparsers):
        """Create and add the parser for the subcommand."""
        parser = subparsers.add_parser(SUBCOMMAND, help=__doc__)

        parser.add_argument("-i", "--installDir", dest="install_dir", required=True,
                            help="Directory to install the download archive. [REQUIRED]")
        parser.add_argument(
            "-l", "--linkDir", dest="link_dir", required=True,
            help="Directory to contain links to all binaries for each version "
            "in the install directory. [REQUIRED]")
        editions = ("base", "enterprise", "targeted")
        parser.add_argument("-e", "--edition", dest="edition", choices=editions, default="base",
                            help="Edition of the build to download, [default: %(default)s].")
        parser.add_argument(
            "-p", "--platform", dest="platform", required=True,
            help="Platform to download [REQUIRED]. Examples include: 'linux', "
            "'ubuntu1804', 'osx', 'rhel62', 'windows'.")
        parser.add_argument(
            "-a", "--architecture", dest="architecture", default="x86_64",
            help="Architecture to download, [default: %(default)s]. Examples include: "
            "'arm64', 'ppc64le', 's390x' and 'x86_64'.")
        parser.add_argument(
            "-u", "--useLatest", dest="use_latest", action="store_true",
            help="If specified, the latest (nightly) version will be downloaded, "
            "if it exists, for the version specified. For example, if specifying "
            "version 3.2 for download, the nightly version for 3.2 will be "
            "downloaded if it exists, otherwise the 'highest' version will be "
            "downloaded, i.e., '3.2.17'")
        parser.add_argument(
            "versions", nargs="*", help=
            "Examples: 4.2 4.2.1 4.4. If 'rc' is included in the version name, we'll use the exact rc, "
            "otherwise we'll pull the highest non-rc version compatible with the version specified."
        )
        parser.add_argument(
            "--evergreen-config", dest="evergreen_config",
            help="Location of evergreen configuration file. If not specified it will look "
            f"for it in the following locations: {EVERGREEN_CONFIG_LOCATIONS}")
        parser.add_argument(
            "--git-token", dest="git_token", help=
            "In most cases it works without git auth. Otherwise you can pass OAth token to increase "
            "the github API rate limit. See https://developer.github.com/v3/#rate-limiting")
