"""The discovery routes ("sources") of the DSC release resolver.

The implementations of the :class:`VersionSource
<buildscripts.resmokelib.multiversion.dsc_release_base.VersionSource>`
interface: the code that encodes external systems, which changes when those
systems change rather than when the resolution policy does. The interface
itself lives in
:mod:`buildscripts.resmokelib.multiversion.dsc_release_base`, the policy and
CLI consuming the sources in
:mod:`buildscripts.resmokelib.multiversion.dsc_release`.

The sources, in ``--source auto`` order:

* ``feed`` (authoritative): the atlas release feed's JSON manifest, which
  directly answers both enumeration and publication;
* ``evg``: git-tag candidates with an Evergreen build-status publication
  proxy -- needs ``viewTasks`` on the DSC projects, which task credentials do
  not have, so it currently 404s everywhere;
* ``tags``: git-tag candidates with the feed's existence probe as the
  publication check.

boto3 and evergreen-py are imported lazily, inside the default factories, so
this module stays importable (and unit-testable) without them.
"""

from __future__ import annotations

import json
from functools import partial
from typing import Callable, Sequence

import structlog

from buildscripts.resmokelib.multiversion.dsc_release_base import (
    ConfigError,
    DscBinaryQuery,
    VersionSource,
)
from buildscripts.resmokelib.multiversion.previous_release_tag import (
    ReleaseTag,
    is_at_or_descending_target,
    list_release_tags,
)

LOGGER = structlog.get_logger(__name__)

# DSC release trains tag their builds with a continuing rc counter that is
# always 4 digits (dsc-release-5 -> r9.0.0-rc1013..rc1020, dsc-release-6 ->
# r9.1.0-rc1021), while mainline rc counters never reach 4 digits -- the
# character-class glob separates the two exactly. If a train ever produces GA
# tags, widen the glob.
DSC_TAG_GLOB = "r*.*.*-rc[0-9][0-9][0-9][0-9]*"

# The Evergreen projects that build the DSC release trains. Not enumerable from
# the public API (plan §8) -- extend when a train is cut.
DSC_EVG_PROJECTS = ("mongodb-mongo-dsc-release-5", "mongodb-mongo-dsc-release-6")

# The DSC projects' build variants that produce the published binaries, mapped
# from the task's multiversion platform/architecture (edition is not encoded in
# the variant names). Verified against the dsc-release-5 project; extend for
# new platforms.
ATLAS_BUILD_VARIANTS = {
    ("amazon2023", "aarch64"): "atlas-amazon2023-arm64",
    ("amazon2023", "x86_64"): "atlas-amazon2023",
    ("ubuntu2204", "aarch64"): "atlas-ubuntu2204-arm64",
}

# Build statuses that mean the binary was produced (and, by release-process
# convention, published to the feed).
EVG_SUCCESS_STATUSES = frozenset({"success", "undisturbing"})

# The atlas release feed: a single JSON manifest in S3 (same schema as the
# public https://downloads.mongodb.org/cloud.json), fetched with the AWS
# credentials the task assumes for exactly this purpose.
ATLAS_FEED_BUCKET = "origin-mongodb-server-atlas"
ATLAS_FEED_KEY = "server/feeds/all.json"

# The module that marks a build as carrying the disagg/atlas storage.
DSC_MODULE = "atlas"


def _default_s3_client() -> object:
    """Build a boto3 S3 client from the ambient credential chain.

    In the task these are the AWS_* variables from the assumed
    access-to-private-atlas-artifacts role. Imported lazily so the module stays
    importable (and unit-testable) without boto3.
    """
    import boto3

    return boto3.client("s3")


class FeedSource(VersionSource):
    """Atlas release-feed route: read the feed's JSON manifest from S3.

    The feed is a single JSON manifest (same schema as the public
    https://downloads.mongodb.org/cloud.json): ``{"versions": [...]}``, newest
    first, each entry carrying a ``version`` string, ``date``, ``githash``,
    release flags, and ``downloads`` keyed by ``arch``/``edition``/``target``
    with archive URLs and hashes -- the same dimensions as the DSC binary
    query. The private feed additionally contains the DSC release-candidate
    builds that the public feed lacks.

    "Published DSC binary" means a download entry for the requested
    platform/architecture/edition carrying the disagg/atlas module (when the
    entry declares its modules).
    """

    name = "feed"
    implemented = True

    def __init__(
        self,
        query: DscBinaryQuery,
        s3_client_factory: Callable[[], object] = _default_s3_client,
        bucket: str = ATLAS_FEED_BUCKET,
        key: str = ATLAS_FEED_KEY,
    ) -> None:
        self.query = query
        self._s3_client_factory = s3_client_factory
        self._s3_client: object = None
        self.bucket = bucket
        self.key = key
        self._manifest: object = None

    def _load_manifest(self) -> object:
        if self._manifest is None:
            s3 = self._get_s3_client()
            response = s3.get_object(Bucket=self.bucket, Key=self.key)
            self._manifest = json.loads(response["Body"].read())
        return self._manifest

    def _get_s3_client(self) -> object:
        if self._s3_client is None:
            self._s3_client = self._s3_client_factory()
        return self._s3_client

    def candidate_versions(self) -> list[str]:
        manifest = self._load_manifest()
        # The manifest is already ordered newest first; preserve its order.
        return [entry["version"] for entry in manifest.get("versions", []) if "version" in entry]

    def has_published_binary(self, version: str) -> bool:
        manifest = self._load_manifest()
        entry = next(
            (item for item in manifest.get("versions", []) if item.get("version") == version),
            None,
        )
        if entry is None:
            LOGGER.debug("Version not in feed", version=version)
            return False
        for download in entry.get("downloads", []):
            if (
                download.get("arch"),
                download.get("edition"),
                download.get("target"),
            ) != (self.query.architecture, self.query.edition, self.query.platform):
                continue
            modules = download.get("modules")
            if modules and DSC_MODULE not in modules:
                LOGGER.debug(
                    "Platform build exists but does not carry the disagg module",
                    version=version,
                    modules=modules,
                )
                return False
            return True
        LOGGER.debug("No platform build in feed", version=version)
        return False


def _default_evg_api() -> object:
    """Build the evergreen-py client from the task's or the user's .evergreen.yml.

    Imported lazily so the module stays importable (and unit-testable) without
    evergreen-py. The repo helper resolves cwd/.evergreen.yml -- where the
    "configure evergreen api credentials" func writes the task's credentials --
    then ~/.evergreen.yml.
    """
    from buildscripts.resmokelib.utils.evergreen_conn import get_evergreen_api

    return get_evergreen_api()


def _enumerate_tag_versions(
    tag_lister: Callable[[], list[ReleaseTag]],
    at_or_descending_head: Callable[[str], bool],
) -> list[tuple[str, str]]:
    """Return (version, commit) pairs for DSC tags, newest first.

    Tags pointing at or descending from HEAD are excluded: the version under
    test is built from HEAD. Ordering is the shared previous_release_tag
    machinery's (git's versioncmp).
    """
    pairs = []
    for release_tag in tag_lister():
        if at_or_descending_head(release_tag.commit):
            LOGGER.info("Tag points at or descends from HEAD; excluded", tag=release_tag.tag)
            continue
        pairs.append((release_tag.version, release_tag.commit))
    return pairs


class EvgVersionSource(VersionSource):
    """EVG route: git-tag candidates, with publication proxied by Evergreen build status.

    The EVG REST API exposes no mongo version string (its version documents
    carry only the git revision, and its version lists include
    built-but-untagged commits -- see §Phase 1.2.1), so candidates come from
    the same git-tag enumeration as the tag route. What EVG adds is the
    publication proxy: for a tag's revision, the DSC project's version
    document, and the ``atlas-*`` build for the requested
    platform/architecture.

    This is a proxy, not the authority: a successful ``atlas-*`` build is
    assumed to be published in the release feed. The feed probe (S3) is the
    authoritative check; db-contrib-tool's actual download is the final word.
    """

    name = "evg"
    implemented = True

    def __init__(
        self,
        query: DscBinaryQuery,
        projects: Sequence[str] = DSC_EVG_PROJECTS,
        api_factory: Callable[[], object] = _default_evg_api,
        tag_lister: Callable[[], list[ReleaseTag]] = partial(
            list_release_tags, pattern=DSC_TAG_GLOB
        ),
        at_or_descending_head: Callable[[str], bool] = is_at_or_descending_target,
    ) -> None:
        self.query = query
        self.projects = tuple(projects)
        self._api_factory = api_factory
        self._api: object = None
        self._tag_lister = tag_lister
        self._at_or_descending_head = at_or_descending_head
        self._commits: dict[str, str] = {}

    def candidate_versions(self) -> list[str]:
        versions = []
        self._commits = {}
        for version, commit in _enumerate_tag_versions(
            self._tag_lister, self._at_or_descending_head
        ):
            self._commits.setdefault(version, commit)
            versions.append(version)
        return versions

    def has_published_binary(self, version: str) -> bool:
        api = self._get_api()
        for project in self.projects:
            if self._project_has_binary(api, project, version):
                return True
        return False

    def _get_api(self) -> object:
        if self._api is None:
            self._api = self._api_factory()
        return self._api

    def _project_has_binary(self, api: object, project: str, version: str) -> bool:
        commit = self._commits.get(version)
        if not commit:
            LOGGER.warning("No commit known for version; cannot check Evergreen", version=version)
            return False
        variant = self._atlas_variant()
        # EVG version ids are "<project with - replaced by _>_<revision>".
        version_id = f"{project.replace('-', '_')}_{commit}"
        try:
            # Read statuses from the version's builds: the version document's
            # build_variants_status is not reliably populated for these projects.
            builds = api.builds_by_version(version_id)
            for build in builds:
                if build.build_variant != variant:
                    continue
                published = build.status in EVG_SUCCESS_STATUSES
                LOGGER.debug(
                    "Evergreen build check",
                    project=project,
                    version=version,
                    variant=variant,
                    status=build.status,
                    published=published,
                )
                return published
        except Exception as exc:
            # A version not existing for this train is the normal "not built
            # here" answer (mainline tags, other trains); genuine API failures
            # are logged loudly and treated the same -- the feed probe (S3) and
            # db-contrib-tool's download remain the authorities.
            LOGGER.warning(
                "Evergreen query failed", project=project, version=version, error=str(exc)
            )
            return False
        LOGGER.debug(
            "No atlas build for variant",
            project=project,
            version=version,
            variant=variant,
        )
        return False

    def _atlas_variant(self) -> str:
        key = (self.query.platform, self.query.architecture)
        try:
            return ATLAS_BUILD_VARIANTS[key]
        except KeyError:
            raise ConfigError(
                f"no atlas build variant known for platform/architecture {key};"
                " extend ATLAS_BUILD_VARIANTS"
            ) from None


class TagSource(VersionSource):
    """Git tag route: enumerate DSC tags, then probe publication against the feed.

    A tag existing is not sufficient (a DSC binary is not necessarily published
    for every tag), so tags are only candidates -- availability is always
    delegated to the feed-backed ``probe``, never trusted from the tag alone.
    Enumeration, ordering and the at/descending-HEAD exclusion are the existing
    ``previous_release_tag`` machinery, shared with the last-patch resolution;
    tags point at commits on release-train branches, so candidates arrive
    newest-first per git's versioncmp.
    """

    name = "tags"
    implemented = True

    def __init__(
        self,
        probe: VersionSource,
        tag_lister: Callable[[], list[ReleaseTag]] = partial(
            list_release_tags, pattern=DSC_TAG_GLOB
        ),
        at_or_descending_head: Callable[[str], bool] = is_at_or_descending_target,
    ) -> None:
        self.probe = probe
        self._tag_lister = tag_lister
        self._at_or_descending_head = at_or_descending_head

    def candidate_versions(self) -> list[str]:
        return [
            version
            for version, _ in _enumerate_tag_versions(self._tag_lister, self._at_or_descending_head)
        ]

    def has_published_binary(self, version: str) -> bool:
        return self.probe.has_published_binary(version)
