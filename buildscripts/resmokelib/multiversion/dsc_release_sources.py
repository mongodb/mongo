"""The discovery routes ("sources") of the DSC release resolver.

The implementations of the :class:`VersionSource
<buildscripts.resmokelib.multiversion.dsc_release_base.VersionSource>`
interface: the code that encodes external systems, which changes when those
systems change rather than when the resolution policy does. The interface
itself lives in
:mod:`buildscripts.resmokelib.multiversion.dsc_release_base`, the policy and
CLI consuming the sources in
:mod:`buildscripts.resmokelib.multiversion.dsc_release`.

The resolver has one discovery route with two sources:

* ``tags``: the DSC train tags, enumerated by the shared previous_release_tag
  machinery (git versionsort, at/descending-HEAD exclusion) -- the candidate
  source;
* ``feed``: the atlas release feed's JSON manifest -- the publication probe
  for those candidates (manifest order is a packaging artifact,
  ``9.1.0-rc1022 < 9.1.0``, not release ordering).

boto3 is imported lazily, inside the default factory, so this module stays
importable (and unit-testable) without it.
"""

from __future__ import annotations

import json
from functools import partial
from typing import Callable, Optional

import structlog

from buildscripts.resmokelib.multiversion.dsc_release_base import (
    DscBinaryQuery,
    VersionSource,
)
from buildscripts.resmokelib.multiversion.previous_release_tag import (
    ReleaseTag,
    find_previous_release_tag,
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

# The atlas release feed: a single JSON manifest in S3 (same schema as the
# public https://downloads.mongodb.org/cloud.json), fetched with the AWS
# credentials the task assumes for exactly this purpose.
ATLAS_FEED_BUCKET = "origin-mongodb-server-atlas"
ATLAS_FEED_KEY = "server/feeds/all.json"

# The module that marks a download as carrying the disagg/atlas storage. The
# feed's DSC builds do not declare modules; when a download declares them, it
# must include this one.
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
    """Atlas release-feed route: the publication probe against the S3 manifest.

    The feed is a single JSON manifest (same schema as the public
    https://downloads.mongodb.org/cloud.json): ``{"versions": [...]}``, newest
    first, each entry carrying a ``version`` string, ``date``, ``githash``,
    release flags, and ``downloads`` keyed by ``arch``/``edition``/``target``
    with archive URLs and hashes -- the same dimensions as the DSC binary
    query. The private feed additionally contains the DSC release-candidate
    builds that the public feed lacks.

    The feed answers one question -- "does ``version`` have a published DSC
    binary for the requested platform/architecture/edition?"; candidate
    enumeration is ``TagSource``'s job (git topology decides the old side).

    "Published DSC binary" means a download entry matching the requested
    platform/architecture/edition -- the same dimension-keyed lookup
    db-contrib-tool performs for the download. A download entry that declares
    its modules must include the disagg/atlas module; entries without module
    info qualify: the feed's DSC builds do not declare modules.
    """

    name = "feed"

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
        # The feed answers one question -- publication -- and its manifest
        # order is packaging, not release ordering (a same-series train rc
        # sorts below the series' GA version: ``9.1.0-rc1022 < 9.1.0``).
        # Candidates come from the git-tag topology (TagSource).
        return []

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
            # Builds are keyed by (arch, edition, target) -- the same lookup
            # db-contrib-tool performs. A download that declares its modules
            # must include the disagg module; entries without module info
            # qualify: the feed's DSC builds do not declare modules.
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


def _enumerate_tag_versions(
    tag_lister: Callable[[], list[ReleaseTag]],
    at_or_descending_head: Callable[[str], bool],
) -> list[ReleaseTag]:
    """Return the DSC tags, newest first (git's versioncmp), excluding tags at
    or descending from HEAD: the version under test is built from HEAD, so such
    tags are not genuinely older than the code under test.
    """
    tags = []
    for release_tag in tag_lister():
        if at_or_descending_head(release_tag.commit):
            LOGGER.info("Tag points at or descends from HEAD; excluded", tag=release_tag.tag)
            continue
        tags.append(release_tag)
    return tags


def _anchor_candidates_at_base(tags: list[ReleaseTag], base: Optional[str]) -> list[ReleaseTag]:
    """Anchor the newest-first candidates at the base tag: the walk starts at
    the base and steps down through the versionsort-older tags.

    The base is the train tag closest in history to HEAD (topological distance:
    git merge-base fork point, then symmetric distance -- see
    :func:`find_previous_release_tag`). Tags sorting newer than the base are
    forked farther away (a superseded train that kept tagging), so the walk
    skips them.

    A ``None`` base means no tag qualifies as the previous release (none match
    the glob, or every one is at/descending HEAD): nothing may be probed. A
    base missing from the enumerated candidates is an invariant violation
    (both use the same glob and repo): raise.
    """
    if base is None:
        return []
    for index, release_tag in enumerate(tags):
        if release_tag.tag == base:
            return tags[index:]
    raise RuntimeError(
        f"previous release tag {base} not among the enumerated candidates;"
        " the tag list and the distance computation disagree"
    )


class TagSource(VersionSource):
    """Git tag route: the candidate source for the DSC release resolver.

    Candidates are the DSC train tags, enumerated newest-first by the shared
    ``previous_release_tag`` machinery (git's versionsort) with tags at or
    descending from HEAD excluded -- the topology bound that makes a candidate
    genuinely older than the code under test, whatever its version string says
    (``9.1.0-rc1022 < 9.1.0`` is a packaging artifact). The walk is anchored at
    the **base**, the train tag closest in history to HEAD (see
    :func:`_anchor_candidates_at_base`).

    Publication is delegated to the feed-backed ``probe``: a tag existing
    does not mean a DSC binary was published for it. The first published
    candidate is normally the base itself; the probe covers "tagged but not
    yet published" by stepping down.
    """

    name = "tags"

    def __init__(
        self,
        probe: VersionSource,
        tag_lister: Callable[[], list[ReleaseTag]] = partial(
            list_release_tags, pattern=DSC_TAG_GLOB
        ),
        at_or_descending_head: Callable[[str], bool] = is_at_or_descending_target,
        previous_tag_finder: Callable[[], Optional[str]] = partial(
            find_previous_release_tag, tag_pattern=DSC_TAG_GLOB
        ),
    ) -> None:
        self.probe = probe
        self._tag_lister = tag_lister
        self._at_or_descending_head = at_or_descending_head
        self._previous_tag_finder = previous_tag_finder

    def candidate_versions(self) -> list[str]:
        tags = _enumerate_tag_versions(self._tag_lister, self._at_or_descending_head)
        base = self._previous_tag_finder()
        LOGGER.info("Anchoring candidates at the previous release tag", base=base)
        anchored = _anchor_candidates_at_base(tags, base)
        return [release_tag.version for release_tag in anchored]

    def has_published_binary(self, version: str) -> bool:
        return self.probe.has_published_binary(version)
