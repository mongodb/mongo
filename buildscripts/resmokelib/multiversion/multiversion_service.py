"""A service for working with multiversion testing."""

from __future__ import annotations

import re
from bisect import bisect_left, bisect_right
from typing import Callable, NamedTuple, Optional

import structlog
import yaml
from packaging.version import Version
from pydantic import BaseModel, Field

from buildscripts.resmokelib.multiversion.previous_release_tag import (
    find_previous_release_tag,
)

VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+")
RELEASE_TAG_RE = re.compile(r"^r\d+\.\d+\.\d+(?:-.+)?$")
LOGGER = structlog.getLogger(__name__)


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
    requires_fcv_tag_list: list[Version]
    requires_fcv_tag_list_continuous: list[Version]
    fcvs_less_than_latest: list[Version]
    eols: list[Version]

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

    def get_fcv_tags_less_than_latest(self) -> list[str]:
        """Get the list of all fcv tags less than the latest."""
        return [tag_str(fcv) for fcv in self.fcvs_less_than_latest]

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

    def get_eols(self) -> list[str]:
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
        mongo_version_yml_file = open(yaml_file, "r", encoding="utf8")
        return cls(**yaml.safe_load(mongo_version_yml_file))

    def get_version(self) -> Version:
        """Get the Version representation of the mongo version being tested."""
        version_match = VERSION_RE.match(self.mongo_version)
        if version_match is None:
            raise ValueError(
                f"Could not determine version from mongo version string '{self.mongo_version}'"
            )
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

    feature_compatibility_versions: list[str] = Field(alias="featureCompatibilityVersions")
    long_term_support_releases: list[str] = Field(alias="longTermSupportReleases")
    eol_versions: list[str] = Field(alias="eolVersions")
    generate_fcv_lower_bound_override: Optional[str] = Field(
        None, alias="generateFCVLowerBoundOverride"
    )

    @classmethod
    def from_yaml_file(cls, yaml_file: str) -> MongoReleases:
        """
        Read the mongo release information from the given yaml file.

        :param yaml_file: Path to yaml file.
        :return: MongoReleases read from file.
        """

        with open(yaml_file, "r", encoding="utf8") as mongo_releases_file:
            yaml_contents = mongo_releases_file.read()
        safe_load_result = yaml.safe_load(yaml_contents)
        try:
            return cls(**safe_load_result)
        except:
            LOGGER.info(
                "MongoReleases.from_yaml_file() failed\n"
                f"yaml_file = {yaml_file}\n"
                f"yaml_contents = {yaml_contents}\n"
                f"safe_load_result = {safe_load_result}"
            )
            raise

    def get_fcv_versions(self) -> list[Version]:
        """Get the Version representation of all fcv versions."""
        return [Version(fcv) for fcv in self.feature_compatibility_versions]

    def get_lts_versions(self) -> list[Version]:
        """Get the Version representation of the lts versions."""
        return [Version(lts) for lts in self.long_term_support_releases]

    def get_eol_versions(self) -> list[Version]:
        """Get the Version representation of the EOL versions."""
        return [Version(eol) for eol in self.eol_versions]


_UNRESOLVED = object()


class MultiversionService:
    """A service for working with multiversion information."""

    def __init__(
        self,
        mongo_version: MongoVersion,
        mongo_releases: MongoReleases,
        last_patch_resolver: Optional[Callable[[str], Optional[str]]] = None,
    ) -> None:
        """
        Initialize the service.

        :param mongo_version: Contents of the Mongo Version file.
        :param mongo_releases: Contents of the Mongo Releases file.
        :param last_patch_resolver: Callable taking a tag glob pattern and
          returning the last patch release tag, or None. Defaults to
          ``find_previous_release_tag`` targeting HEAD. Tests can inject a fake
          to avoid invoking git.
        """
        self.mongo_version = mongo_version
        self.mongo_releases = mongo_releases
        self.last_patch_resolver = last_patch_resolver or (
            lambda tag_pattern: find_previous_release_tag("HEAD", tag_pattern=tag_pattern)
        )
        self._last_patch_cache: object = _UNRESOLVED

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
        requires_fcv_tag_list = fcvs[bisect_right(fcvs, last_lts) : bisect_right(fcvs, latest)]
        requires_fcv_tag_list_continuous = [latest]

        # All FCVs less than latest.
        fcvs_less_than_latest = fcvs[: bisect_left(fcvs, latest)]

        return VersionConstantValues(
            latest=latest,
            last_continuous=last_continuous,
            last_lts=last_lts,
            requires_fcv_tag_list=requires_fcv_tag_list,
            requires_fcv_tag_list_continuous=requires_fcv_tag_list_continuous,
            fcvs_less_than_latest=fcvs_less_than_latest,
            eols=eols,
        )

    def get_last_patch_version(self) -> Optional[str]:
        """Get the last patch version (e.g. '8.3.1' or '8.3.1-rc1010').

        Resolved lazily on first call by walking git tags, then memoized for
        the lifetime of the service. Returns ``None`` if no matching tag
        exists or the resolver fails (e.g. no git, malformed tag).
        """
        # Cached `None` is a valid resolved value, so we use a sentinel to
        # distinguish it from "not computed yet".
        if self._last_patch_cache is _UNRESOLVED:
            self._last_patch_cache = self._resolve_last_patch()
        return self._last_patch_cache  # type: ignore[return-value]

    def get_last_patch_fcv(self) -> Optional[str]:
        """Get the FCV derived from the last patch tag (e.g. '8.3')."""
        last_patch = self.get_last_patch_version()
        if last_patch is None:
            return None
        major, minor, _ = last_patch.split(".", 2)
        return f"{major}.{minor}"

    def _resolve_last_patch(self) -> Optional[str]:
        latest = self.mongo_version.get_version()
        tag_pattern = f"r{latest.major}.{latest.minor}.*"
        try:
            last_patch_tag = self.last_patch_resolver(tag_pattern)
        except Exception as exc:
            LOGGER.info("Failed to resolve last patch tag", tag_pattern=tag_pattern, error=exc)
            return None
        if not last_patch_tag:
            return None
        if not RELEASE_TAG_RE.match(last_patch_tag):
            LOGGER.info(
                "Unrecognized format for last patch tag", tag=last_patch_tag, pattern=tag_pattern
            )
            return None
        return last_patch_tag[1:]

    def get_last_patch_binary_name(self, base_name: str) -> str:
        """
        Return the name of the binary file corresponding to last-patch release of the given tool.

          :param base_name: Base name of binary (mongod, mongos, etc...).
          :return: Name of last patch version of the given tool.
        """
        last_patch_fcv = self.get_last_patch_fcv()
        return f"{base_name}-{last_patch_fcv}"
