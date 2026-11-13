"""A service for working with multiversion testing."""

from __future__ import annotations

import os
import re
from abc import ABC, abstractmethod
from bisect import bisect_left, bisect_right
from functools import cache
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

    @classmethod
    def calculate(
        cls, mongo_version: MongoVersion, mongo_releases: MongoReleases
    ) -> VersionConstantValues:
        """Calculate constants from version/release data, independently of service construction."""
        latest = mongo_version.get_version()
        fcvs = mongo_releases.get_fcv_versions()
        lts = mongo_releases.get_lts_versions()
        eols = mongo_releases.get_eol_versions()

        # Highest release less than latest.
        last_continuous = fcvs[bisect_left(fcvs, latest) - 1]

        # Highest LTS release less than latest.
        last_lts = lts[bisect_left(lts, latest) - 1]

        # All FCVs greater than last LTS, up to latest.
        requires_fcv_tag_list = fcvs[bisect_right(fcvs, last_lts) : bisect_right(fcvs, latest)]
        requires_fcv_tag_list_continuous = [latest]

        # All FCVs less than latest.
        fcvs_less_than_latest = fcvs[: bisect_left(fcvs, latest)]

        return cls(
            latest=latest,
            last_continuous=last_continuous,
            last_lts=last_lts,
            requires_fcv_tag_list=requires_fcv_tag_list,
            requires_fcv_tag_list_continuous=requires_fcv_tag_list_continuous,
            fcvs_less_than_latest=fcvs_less_than_latest,
            eols=eols,
        )

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

    def get_last_versions(self) -> list[str]:
        """Return the supported last-LTS and last-continuous multiversion options."""
        last_lts = self.get_last_lts_fcv()
        last_continuous = self.get_last_continuous_fcv()
        last_versions = [config.MultiversionOptions.LAST_LTS]
        if last_continuous != last_lts and last_continuous not in self.get_eols():
            last_versions.append(config.MultiversionOptions.LAST_CONTINUOUS)
        return last_versions


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
        last_patch_behavior: LastPatchBehavior,
    ) -> None:
        """
        Initialize the service.

        :param mongo_version: Contents of the Mongo Version file.
        :param mongo_releases: Contents of the Mongo Releases file.
        :param last_patch_behavior: The ASC or DSC behavior selected at construction.
        """
        self.mongo_version = mongo_version
        self.mongo_releases = mongo_releases
        self.last_patch_behavior = last_patch_behavior
        self._version_constants = VersionConstantValues.calculate(mongo_version, mongo_releases)

    def get_version_constants(self) -> VersionConstantValues:
        """Get the constants calculated for this service's fixed configuration."""
        return self._version_constants

    def get_last_patch_version(self) -> Optional[str]:
        """Get the version to use for last-patch testing (e.g. '8.0.29').

        The selected behavior resolves a git tag for ASC or the recorded requested release for
        DSC. A requested release is not proof of the artifact installed after download fallback.

        Resolved lazily on first call, then memoized for the lifetime of the
        service. ASC returns ``None`` when the series has nothing to upgrade from;
        DSC raises LastPatchSelectionError when required selection metadata is unavailable.
        """
        return self.last_patch_behavior.get_version()

    def get_last_patch_fcv(self) -> Optional[str]:
        """Get the FCV derived from the selected old release (e.g. '8.3')."""
        return self.last_patch_behavior.get_fcv()

    def get_supported_fcv_for_last_patch_bin(self) -> str:
        """Return last-continuous FCV when supported, falling back to last-LTS.

        A recorded last-LTS release remains a supported peer after the target advances.
        Reject other old releases that cannot support last-continuous FCV.
        """
        versions = self.get_version_constants()
        last_continuous_fcv = versions.get_last_continuous_fcv()
        old_fcv = self.get_last_patch_fcv()
        if old_fcv is not None and Version(old_fcv) >= Version(last_continuous_fcv):
            return last_continuous_fcv
        last_lts_fcv = versions.get_last_lts_fcv()
        if old_fcv == last_lts_fcv:
            return last_lts_fcv
        raise ValueError(
            f"Selected last-patch binary supports FCV {old_fcv}, "
            f"but FCV {last_continuous_fcv} is required"
        )

    def has_released_patch_version(self) -> bool:
        """Return whether there is a last-patch version to offer as a default peer.

        Deliberately strict and independent of the environment: this decides whether
        LAST_PATCH joins `last_versions` for every suite that declares it, and it runs
        during task generation, where the general suites' opt-in is not set. A variant
        that wants last-patch on a series with no GA release overrides `last_versions`
        directly, which is how the disagg suites run on a pre-release series.
        """
        return self.last_patch_behavior.has_released_patch_version()

    def get_binary_name_for_version(self, version: str, base_name: str) -> str:
        """Return the old binary name (e.g. 'mongod-8.0') for a multiversion option.

        :param version: One of the config.MultiversionOptions constants (LAST_LTS,
            LAST_CONTINUOUS, LAST_PATCH).
        :param base_name: The binary base name, e.g. 'mongod' or 'mongos'.
        """
        version_constants = self.get_version_constants()
        suffix_for_option = {
            config.MultiversionOptions.LAST_LTS: version_constants.get_last_lts_fcv,
            config.MultiversionOptions.LAST_CONTINUOUS: version_constants.get_last_continuous_fcv,
            config.MultiversionOptions.LAST_PATCH: self.last_patch_behavior.get_bin_suffix,
        }
        if version not in suffix_for_option:
            raise ValueError(f"Unknown multiversion option: {version}")
        suffix = suffix_for_option[version]()
        if suffix is None:
            raise ValueError("No eligible last-patch release is available for executable naming")
        return f"{base_name}-{suffix}"

    def get_last_versions(self) -> list[str]:
        """Return the last release version names to use in multiversion testing.

        These are the latest release versions from which an upgrade to the current release is
        supported. Returns ``[LAST_LTS]`` when last-continuous equals last-LTS or is EOL'd,
        otherwise ``[LAST_LTS, LAST_CONTINUOUS]``.
        """
        return self._version_constants.get_last_versions()


class LastPatchBehavior(ABC):
    """Source-specific last-patch resolution for one invocation."""

    def __init__(self, mongo_version: MongoVersion) -> None:
        self.mongo_version = mongo_version

    @abstractmethod
    def get_version(self) -> Optional[str]:
        """Return the requested old release, or None when ASC has no eligible release."""

    @abstractmethod
    def get_bin_suffix(self) -> Optional[str]:
        """Return the suffix used for the old executable."""

    def has_released_patch_version(self) -> bool:
        """Preserve the current-series GA eligibility rule used by task generation."""
        return self._resolve_last_patch_tag(exclude_prerelease=True) is not None

    def get_fcv(self) -> Optional[str]:
        """Derive the old FCV from the resolved release."""
        version = self.get_version()
        if version is None:
            return None
        major, minor, _ = version.split(".", 2)
        return f"{major}.{minor}"

    def get_metadata(self) -> dict:
        """Return available artifact metadata."""
        return {}

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
            last_patch_tag = find_previous_release_tag(
                "HEAD", tag_pattern=tag_pattern, exclude_prerelease=exclude_prerelease
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


class ASCLastPatchBehavior(LastPatchBehavior):
    """The existing ASC git-tag resolution."""

    def __init__(self, mongo_version: MongoVersion) -> None:
        super().__init__(mongo_version)
        self._last_patch_cache: object = _UNRESOLVED

    def get_version(self) -> Optional[str]:
        # Cached None is a valid result.
        if self._last_patch_cache is _UNRESOLVED:
            self._last_patch_cache = self._resolve_last_patch()
        return self._last_patch_cache  # type: ignore[return-value]

    def get_bin_suffix(self) -> Optional[str]:
        return self.get_fcv()

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


class LastPatchSelectionError(ValueError):
    """The required DSC selection is missing or inconsistent."""


class DSCLastPatchBehavior(LastPatchBehavior):
    """Resolve the old DSC binary exclusively from the recorded download manifest."""

    def __init__(
        self,
        mongo_version: MongoVersion,
        mongo_releases: MongoReleases,
        manifest_loader: Callable[[], list[dict]],
    ) -> None:
        super().__init__(mongo_version)
        self.mongo_releases = mongo_releases
        self.manifest_loader = manifest_loader

    @cache
    def get_metadata(self) -> dict:
        """Read and validate the selected entry once; retain available artifact identities."""
        try:
            downloads = self.manifest_loader()
            if not isinstance(downloads, list):
                raise ValueError("expected a list of downloads")
            entries = [
                entry
                for entry in downloads
                if entry.get("discovery_request", {}).get("request_type")
                == "mongo_last_patch_version"
            ]
            if not entries:
                raise ValueError("missing selected last-patch download")
            if len(entries) != 1:
                raise ValueError("multiple last-patch downloads")
            entry = entries[0]
            release = entry["discovery_request"]["identifier"]
            suffix = entry["bin_suffix"]
            if not RELEASE_TAG_RE.fullmatch(f"r{release}"):
                raise ValueError(f"invalid requested release {release!r}")
            if not re.fullmatch(r"[a-zA-Z0-9_.-]+", suffix):
                raise ValueError(f"invalid executable suffix {suffix!r}")
            series = ".".join(release.split(".")[:2])
            suffix_series = VERSION_RE.match(suffix)
            if suffix_series and suffix_series.group() != series:
                raise ValueError("selected series and executable suffix disagree")
            if (
                series not in self.mongo_releases.feature_compatibility_versions
                or Version(series) > self.mongo_version.get_version()
            ):
                raise ValueError(f"unsupported selected series {series}")
            if len([other for other in downloads if other.get("bin_suffix") == suffix]) > 1:
                raise ValueError(f"multiple downloads use suffix {suffix!r}")
            return entry
        except (AttributeError, KeyError, OSError, TypeError, ValueError) as exc:
            raise LastPatchSelectionError(f"Invalid DSC last-patch metadata: {exc}") from exc

    def get_version(self) -> str:
        return self.get_metadata()["discovery_request"]["identifier"]

    def get_bin_suffix(self) -> str:
        return self.get_metadata()["bin_suffix"]
