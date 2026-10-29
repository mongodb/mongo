"""A service for working with multiversion testing."""

from __future__ import annotations

import os
import re
from bisect import bisect_left, bisect_right
from typing import Callable, NamedTuple, Optional

import structlog
import yaml
from packaging.version import Version
from pydantic import BaseModel, Field

from buildscripts.resmokelib import config
from buildscripts.resmokelib.multiversion.previous_release_tag import (
    find_previous_release_tag,
)

VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+")
RELEASE_TAG_RE = re.compile(r"^r\d+\.\d+\.\d+(?:-.+)?$")
# Same, but final (GA) only: `r8.0.29`, not `r8.0.30-rc0` or `r8.0.13-s8-0`.
FINAL_RELEASE_TAG_RE = re.compile(r"^r\d+\.\d+\.\d+$")

# Set by evergreen/multiversion_selection.sh for the general last-patch suites, which
# must upgrade from a released version. Unset means the permissive behaviour the
# disagg/DSC suites rely on: they test against Atlas release candidates, which are never
# final releases.
LAST_PATCH_EXCLUDE_PRERELEASE_ENV_VAR = "MULTIVERSION_LAST_PATCH_EXCLUDE_PRERELEASE"
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
        :param last_patch_resolver: Callable taking a tag glob pattern and a
          ``exclude_prerelease`` flag, returning the last patch release tag or None. Defaults to
          ``find_previous_release_tag`` targeting HEAD. Tests can inject a fake to
          avoid invoking git.
        """
        self.mongo_version = mongo_version
        self.mongo_releases = mongo_releases
        self.last_patch_resolver = last_patch_resolver or (
            lambda tag_pattern, exclude_prerelease=False: find_previous_release_tag(
                "HEAD", tag_pattern=tag_pattern, exclude_prerelease=exclude_prerelease
            )
        )
        self._last_patch_cache: object = _UNRESOLVED
        self._version_constants: Optional[VersionConstantValues] = None

    def get_version_constants(self) -> VersionConstantValues:
        """Get the multiversion constants, computing them once on first call."""
        if self._version_constants is None:
            self._version_constants = self._calculate_version_constants()
        return self._version_constants

    def _calculate_version_constants(self) -> VersionConstantValues:
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
        """Get the version to use for last-patch testing (e.g. '8.0.29').

        This is the newest *published* release of the current series, bounded by
        the newest release tag reachable from HEAD so that a build is never
        tested against a release newer than itself.

        Resolved lazily on first call, then memoized for the lifetime of the
        service. Returns ``None`` when the series has nothing to upgrade from --
        which callers treat as "skip last-patch testing".
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

    def _current_series_tag_pattern(self) -> str:
        """Return a git tag glob matching every tag in the current release series."""
        latest = self.mongo_version.get_version()
        return f"r{latest.major}.{latest.minor}.*"

    def _resolve_last_patch_tag(self, exclude_prerelease: bool) -> Optional[str]:
        """Return the newest release tag of this series before HEAD, without its 'r'.

        `find_previous_release_tag` never returns a tag at or descending from HEAD, so
        the result is always an earlier release -- the build under test can never be
        compared against itself. That is also what makes a freshly cut branch skip: on a
        v9.0 whose HEAD is still `r9.0.0`, the tag is not *before* HEAD, so nothing
        resolves. Once commits land past it, `r9.0.0` becomes the version to test
        against, which is exactly right: HEAD is the code that will become 9.0.1.
        """
        tag_pattern = self._current_series_tag_pattern()
        try:
            last_patch_tag = self.last_patch_resolver(
                tag_pattern, exclude_prerelease=exclude_prerelease
            )
        except Exception as exc:
            LOGGER.info("Failed to resolve last patch tag", tag_pattern=tag_pattern, error=exc)
            return None
        if not last_patch_tag:
            # No release tag of this series precedes HEAD. Either the series has never
            # released (master, whose only in-series tag is an alpha) or the branch was
            # just cut and HEAD is still the release tag itself. Skip rather than fail:
            # last-lts and last-continuous share the same selection task and must not be
            # taken down with it.
            LOGGER.info(
                "No release tag of this series precedes HEAD; skipping last-patch",
                tag_pattern=tag_pattern,
                exclude_prerelease=exclude_prerelease,
            )
            return None
        expected = FINAL_RELEASE_TAG_RE if exclude_prerelease else RELEASE_TAG_RE
        if not expected.match(last_patch_tag):
            LOGGER.info(
                "Unrecognized format for last patch tag",
                tag=last_patch_tag,
                pattern=tag_pattern,
                exclude_prerelease=exclude_prerelease,
            )
            return None
        return last_patch_tag[1:]

    def _resolve_last_patch(self) -> Optional[str]:
        """Resolve the version to hand to db-contrib-tool for last-patch.

        The newest *final* release tag of this series before HEAD, unless the caller
        opted into the permissive behaviour the disagg/DSC suites need (see
        :data:`LAST_PATCH_EXCLUDE_PRERELEASE_ENV_VAR`). Those deliberately test against Atlas
        release candidates, so restricting them to GA tags would leave them nothing.

        Restricting the general suites to final tags does two things. It keeps master
        out, since `r9.1.0-alpha0` is not a release anyone runs. And once a release
        candidate for the *next* patch is cut, it stops that rc -- which is nearer HEAD
        than the last actual release -- from being preferred over it.

        The resolved tag may not be published yet; that is deliberate. db-contrib-tool
        resolves it to that commit's Evergreen build, and steps down through published
        releases at or below it when that build is missing. Determining publication here
        would mean a second implementation of that policy on the other side of one
        interface.
        """
        return self._resolve_last_patch_tag(exclude_prerelease=self._exclude_prerelease())

    def _exclude_prerelease(self) -> bool:
        """Return whether last-patch must resolve to a final (GA) release tag.

        Off by default, which is the permissive behaviour the disagg/DSC suites rely on.
        The general suites opt in from evergreen/multiversion_selection.sh, because
        db-contrib-tool builds the `multiversion-config` command line itself and cannot
        be told to pass a flag.
        """
        return bool(os.environ.get(LAST_PATCH_EXCLUDE_PRERELEASE_ENV_VAR, "").strip())

    def has_released_patch_version(self) -> bool:
        """Return whether there is a last-patch version to offer as a default peer.

        Deliberately strict and independent of the environment: this decides whether
        LAST_PATCH joins `last_versions` for every suite that declares it, and it runs
        during task generation, where the general suites' opt-in is not set. A variant
        that wants last-patch on a series with no GA release overrides `last_versions`
        directly, which is how the disagg suites run on a pre-release series.
        """
        return self._resolve_last_patch_tag(exclude_prerelease=True) is not None

    def get_binary_name_for_version(self, version: str, base_name: str) -> str:
        """Return the old binary name (e.g. 'mongod-8.0') for a multiversion option.

        :param version: One of the config.MultiversionOptions constants (LAST_LTS,
            LAST_CONTINUOUS, LAST_PATCH).
        :param base_name: The binary base name, e.g. 'mongod' or 'mongos'.
        """
        version_constants = self.get_version_constants()
        fcv_for_option = {
            config.MultiversionOptions.LAST_LTS: version_constants.get_last_lts_fcv,
            config.MultiversionOptions.LAST_CONTINUOUS: version_constants.get_last_continuous_fcv,
            config.MultiversionOptions.LAST_PATCH: self.get_last_patch_fcv,
        }
        if version not in fcv_for_option:
            raise ValueError(f"Unknown multiversion option: {version}")
        return f"{base_name}-{fcv_for_option[version]()}"

    def get_last_versions(self) -> list[str]:
        """Return the last release version names to use in multiversion testing.

        These are the latest release versions from which an upgrade to the current release is
        supported. Returns ``[LAST_LTS]`` when last-continuous equals last-LTS or is EOL'd,
        otherwise ``[LAST_LTS, LAST_CONTINUOUS]``.
        """
        version_constants = self.get_version_constants()
        last_lts_fcv = version_constants.get_last_lts_fcv()
        last_continuous_fcv = version_constants.get_last_continuous_fcv()
        if (
            last_continuous_fcv == last_lts_fcv
            or last_continuous_fcv in version_constants.get_eols()
        ):
            return [config.MultiversionOptions.LAST_LTS]
        return [
            config.MultiversionOptions.LAST_LTS,
            config.MultiversionOptions.LAST_CONTINUOUS,
        ]
