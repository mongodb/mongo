"""A service for working with multiversion testing."""
from __future__ import annotations
import http
import os

import re
from bisect import bisect_left, bisect_right
from subprocess import DEVNULL, STDOUT, CalledProcessError, call, check_output
from typing import List, NamedTuple, Optional

from packaging.version import Version
from pydantic import BaseModel, Field
import requests
import retry
import structlog
import yaml
import buildscripts.resmokelib.config as _config

# These values must match the include paths for artifacts.tgz in evergreen.yml.
MONGO_VERSION_YAML = ".resmoke_mongo_version.yml"
VERSION_RE = re.compile(r'^[0-9]+\.[0-9]+')
LOGGER = structlog.getLogger(__name__)
RELEASES_LOCAL_FILE = os.path.join("src", "mongo", "util", "version", "releases.yml")
# We use the "releases.yml" file from "master" because it is guaranteed to be up-to-date
# with the latest EOL versions. If a "last-continuous" version is EOL, we don't include
# it in the multiversion config and therefore don't test against it.
MASTER_RELEASES_REMOTE_FILE = "https://raw.githubusercontent.com/mongodb/mongo/master/src/mongo/util/version/releases.yml"


def get_mongo_git_version():
    """Generate the mongo git description. Should only be called in the root of the mongo directory."""
    try:
        res = check_output("git describe", shell=True, text=True)
    except CalledProcessError as exp:
        raise ChildProcessError("Failed to run git describe to get the latest tag") from exp

    # E.g. res = 'r5.1.0-alpha-597-g8c345c6693\n'
    return res[1:]  # Remove the leading "r" character.


@retry.retry(tries=5, delay=3)
def get_releases_file_from_remote():
    """Get the latest releases.yml from github."""
    try:
        response = requests.get(MASTER_RELEASES_REMOTE_FILE)
        if response.status_code != http.HTTPStatus.OK:
            raise RuntimeError("Http response for releases yml file was not 200 but was " +
                               response.status_code)
        LOGGER.info(f"Got releases.yml file remotely: {MASTER_RELEASES_REMOTE_FILE}")
        return response.content
    except Exception as exc:
        LOGGER.warning(f"Could not get releases.yml file remotely: {MASTER_RELEASES_REMOTE_FILE}")
        raise exc


def get_releases_file_locally_or_fallback_to_remote():
    """Get the latest releases.yml locally or fallback to getting it from github."""
    if os.path.exists(RELEASES_LOCAL_FILE):
        LOGGER.info(f"Found releases.yml file locally: {RELEASES_LOCAL_FILE}")
        with open(RELEASES_LOCAL_FILE, 'r') as file:
            return file.read()
    else:
        LOGGER.warning(f"Could not find releases.yml file locally: {RELEASES_LOCAL_FILE}")
        return get_releases_file_from_remote()


def in_git_root_dir():
    """Return True if we are in the root of a git directory."""
    if call(["git", "branch"], stderr=STDOUT, stdout=DEVNULL) != 0:
        # We are not in a git directory.
        return False

    git_root_dir = check_output("git rev-parse --show-toplevel", shell=True, text=True).strip()
    # Always use forward slash for the cwd path to resolve inconsistent formatting with Windows.
    curr_dir = os.getcwd().replace("\\", "/")
    return git_root_dir == curr_dir


def tag_str(version: Version) -> str:
    """Return a tag for the given version."""
    return f"requires_fcv_{version.major}{version.minor}"


def version_str(version: Version) -> str:
    """Return a string of the given version in 'MAJOR.MINOR' form."""
    return f"{version.major}.{version.minor}"


class VersionConstantValues(NamedTuple):
    """
    Object to hold the calculated Version constants.

    * latest: Latest FCV.
    * last_continuous: Last continuous FCV.
    * last_lts: Last LTS FCV.
    * requires_fcv_tag_list: List of FCVs that we need to generate a tag for against LTS versions.
    * requires_fcv_tag_list_continuous: List of FCVs that we need to generate a tag for against
      continuous versions.
    * fcvs_less_than_latest: List of all FCVs that are less than latest, starting from v4.0.
    * eols: List of stable MongoDB versions since v2.0 that have been EOL'd.
    """

    latest: Version
    last_continuous: Version
    last_lts: Version
    requires_fcv_tag_list: List[Version]
    requires_fcv_tag_list_continuous: List[Version]
    fcvs_less_than_latest: List[Version]
    eols: List[Version]

    def get_fcv_tag_list(self) -> str:
        """Get a comma joined string of all the fcv tags."""
        return ",".join([tag_str(tag) for tag in self.requires_fcv_tag_list])

    def get_lts_fcv_tag_list(self) -> str:
        """Get a comma joined string of all the LTS fcv tags."""
        # Note: the LTS tag list is the default used above, so this function is the same as
        # `get_fcv_tag_list`. This function was added to make it explicit when we want to use
        # the LTS version vs the default.
        return ",".join([tag_str(tag) for tag in self.requires_fcv_tag_list])

    def get_continuous_fcv_tag_list(self) -> str:
        """Get a comma joined string of all the continuous fcv tags."""
        return ",".join([tag_str(tag) for tag in self.requires_fcv_tag_list_continuous])

    def get_latest_tag(self) -> str:
        """Get a string version of the latest FCV."""
        return tag_str(self.latest)

    def get_last_lts_fcv(self) -> str:
        """Get a string version of the last LTS FCV."""
        return version_str(self.last_lts)

    def get_last_continuous_fcv(self) -> str:
        """Get a string version of the last continuous FCV."""
        return version_str(self.last_continuous)

    def get_latest_fcv(self) -> str:
        """Get a string version of the latest FCV."""
        return version_str(self.latest)

    def build_last_lts_binary(self, base_name: str) -> str:
        """
        Build the name of the binary that the LTS version of the given tool will have.

        :param base_name: Base name of binary (mongo, mongod, mongos).
        :return: Name of LTS version of the given tool.
        """
        last_lts = self.get_last_lts_fcv()
        return f"{base_name}-{last_lts}"

    def build_last_continuous_binary(self, base_name: str) -> str:
        """
        Build the name of the binary that the continuous version of the given tool will have.

        :param base_name: Base name of binary (mongo, mongod, mongos).
        :return: Name of continuous version of the given tool.
        """
        last_continuous = self.get_last_continuous_fcv()
        return f"{base_name}-{last_continuous}"

    def get_eols(self) -> List[str]:
        """Get EOL'd versions as list of strings."""
        return [version_str(eol) for eol in self.eols]


class MongoVersion(BaseModel):
    """
    The mongo version being tested.

    * mongo_version: The mongo version being tested.
    """

    mongo_version: str

    @classmethod
    def generate(cls) -> MongoReleases:
        if in_git_root_dir():
            return cls(mongo_version=get_mongo_git_version())

        with open(MONGO_VERSION_YAML, 'r') as file:
            mongo_version_object = yaml.safe_load(file)
            return cls(mongo_version=mongo_version_object["mongo_version"])

    def get_version(self) -> Version:
        """Get the Version representation of the mongo version being tested."""
        version_match = VERSION_RE.match(self.mongo_version)
        if version_match is None:
            raise ValueError(
                f"Could not determine version from mongo version string '{self.mongo_version}'")
        return Version(version_match.group(0))


class MongoReleases(BaseModel):
    """
    Information about the FCVs and LTS release version since v4.0.

    * feature_compatibility_version: All FCVs starting with 4.0.
    * long_term_support_releases: All LTS releases starting with 4.0.
    * eol_versions: List of stable MongoDB versions since 2.0 that have been EOL'd.
    * generate_fcv_lower_bound_override: Extend FCV generation down to the previous value of last
      LTS.
    """

    feature_compatibility_versions: List[str] = Field(alias="featureCompatibilityVersions")
    long_term_support_releases: List[str] = Field(alias="longTermSupportReleases")
    eol_versions: List[str] = Field(alias="eolVersions")
    generate_fcv_lower_bound_override: Optional[str] = Field(None,
                                                             alias="generateFCVLowerBoundOverride")

    @classmethod
    def generate(cls) -> MongoReleases:
        """
        Read the mongo release information.

        :return: MongoReleases parsed mongo release versions
        """

        if _config.EVERGREEN_TASK_ID:
            yaml_contents = get_releases_file_from_remote()
        else:
            yaml_contents = get_releases_file_locally_or_fallback_to_remote()
        safe_load_result = yaml.safe_load(yaml_contents)
        try:
            return cls(**safe_load_result)
        except:
            LOGGER.info("MongoReleases.generate() failed\n"
                        f"yaml_contents = {yaml_contents}\n"
                        f"safe_load_result = {safe_load_result}")
            raise

    def get_fcv_versions(self) -> List[Version]:
        """Get the Version representation of all fcv versions."""
        return [Version(fcv) for fcv in self.feature_compatibility_versions]  # pylint: disable=not-an-iterable

    def get_lts_versions(self) -> List[Version]:
        """Get the Version representation of the lts versions."""
        return [Version(lts) for lts in self.long_term_support_releases]  # pylint: disable=not-an-iterable

    def get_eol_versions(self) -> List[Version]:
        """Get the Version representation of the EOL versions."""
        return [Version(eol) for eol in self.eol_versions]  # pylint: disable=not-an-iterable


class MultiversionService:
    """A service for working with multiversion information."""

    def __init__(self, mongo_version: MongoVersion, mongo_releases: MongoReleases) -> None:
        """
        Initialize the service.

        :param mongo_version: Contents of the Mongo Version file.
        :param mongo_releases: Contents of the Mongo Releases file.
        """
        self.mongo_version = mongo_version
        self.mongo_releases = mongo_releases

    def calculate_version_constants(self) -> VersionConstantValues:
        """Calculate multiversion constants from data files."""
        latest = self.mongo_version.get_version()
        fcvs = self.mongo_releases.get_fcv_versions()
        lts = self.mongo_releases.get_lts_versions()
        eols = self.mongo_releases.get_eol_versions()

        # Highest release less than latest.
        last_continuous = fcvs[bisect_left(fcvs, latest) - 1]

        # Highest LTS release less than latest.
        last_lts = lts[bisect_left(lts, latest) - 1]

        # All FCVs greater than last LTS, up to latest.
        requires_fcv_tag_list = fcvs[bisect_right(fcvs, last_lts):bisect_right(fcvs, latest)]
        requires_fcv_tag_list_continuous = [latest]

        # All FCVs less than latest.
        fcvs_less_than_latest = fcvs[:bisect_left(fcvs, latest)]

        return VersionConstantValues(
            latest=latest,
            last_continuous=last_continuous,
            last_lts=last_lts,
            requires_fcv_tag_list=requires_fcv_tag_list,
            requires_fcv_tag_list_continuous=requires_fcv_tag_list_continuous,
            fcvs_less_than_latest=fcvs_less_than_latest,
            eols=eols,
        )
