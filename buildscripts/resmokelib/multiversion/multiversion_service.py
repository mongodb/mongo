"""A service for working with multiversion testing."""
from __future__ import annotations

import re
from bisect import bisect_left, bisect_right
from typing import List, NamedTuple, Optional

from packaging.version import Version
from pydantic import BaseModel, Field
import yaml

# These values must match the include paths for artifacts.tgz in evergreen.yml.
MONGO_VERSION_YAML = ".resmoke_mongo_version.yml"
RELEASES_YAML = ".resmoke_mongo_release_values.yml"
VERSION_RE = re.compile(r'^[0-9]+\.[0-9]+')


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
    def from_yaml_file(cls, yaml_file: str) -> MongoVersion:
        """
        Read the mongo version from the given yaml file.

        :param yaml_file: Path to yaml file.
        :return: MongoVersion read from file.
        """
        mongo_version_yml_file = open(yaml_file, 'r')
        return cls(**yaml.safe_load(mongo_version_yml_file))

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
    def from_yaml_file(cls, yaml_file: str) -> MongoReleases:
        """
        Read the mongo release information from the given yaml file.

        :param yaml_file: Path to yaml file.
        :return: MongoReleases read from file.
        """

        mongo_releases_file = open(yaml_file, 'r')
        return cls(**yaml.safe_load(mongo_releases_file))

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
