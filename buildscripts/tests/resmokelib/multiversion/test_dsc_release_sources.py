"""Unit tests for the DSC release resolver's discovery routes.

Covers buildscripts/resmokelib/multiversion/dsc_release_sources.py: each
source encodes one external system (the atlas feed's S3 manifest, the git tag
listing) and is tested against a fake of that system. The resolution policy
consuming these sources is tested in test_dsc_release.py.
"""

import io
import json
import unittest

import buildscripts.resmokelib.multiversion.dsc_release_sources as under_test
import buildscripts.resmokelib.multiversion.previous_release_tag as previous_release_tag
from buildscripts.resmokelib.multiversion.dsc_release_base import (
    DscBinaryQuery,
    VersionSource,
)
from buildscripts.resmokelib.multiversion.previous_release_tag import ReleaseTag


def release_tag(name, commit):
    return ReleaseTag(tag=name, version=name[1:], commit=commit)


class FakeProbe(VersionSource):
    """A stand-in for the feed-backed probe with canned publication answers."""

    def __init__(self, published=()):
        self._published = set(published)
        self.probed = []

    def candidate_versions(self):
        return []

    def has_published_binary(self, version):
        self.probed.append(version)
        return version in self._published


class TestTagSource(unittest.TestCase):
    def test_candidates_are_git_tags_without_the_r_prefix_in_git_order(self):
        # Ordering is the shared previous_release_tag machinery's job (git's
        # versionsort); the source strips the 'r' prefix, filters, and anchors.
        tags = [release_tag("r9.0.0-rc1020", "commit_b"), release_tag("r9.0.0-rc1013", "commit_a")]
        source = under_test.TagSource(
            probe=FakeProbe(),
            tag_lister=lambda: tags,
            at_or_descending_head=lambda commit: False,
            previous_tag_finder=lambda: "r9.0.0-rc1020",
        )
        self.assertEqual(source.candidate_versions(), ["9.0.0-rc1020", "9.0.0-rc1013"])

    def test_tags_at_or_descending_head_are_excluded(self):
        # A tag pointing at HEAD (the build under test) or on a commit built
        # after it must never be the old side, even though its version is
        # older than the version under test.
        tags = [
            release_tag("r9.0.0-rc2000", "descendant"),
            release_tag("r9.0.0-rc1020", "ancestor"),
            release_tag("r9.1.0-rc1021", "head"),
        ]
        at_or_descending = {"descendant", "head"}
        source = under_test.TagSource(
            probe=FakeProbe(),
            tag_lister=lambda: tags,
            at_or_descending_head=lambda commit: commit in at_or_descending,
            previous_tag_finder=lambda: "r9.0.0-rc1020",
        )
        self.assertEqual(source.candidate_versions(), ["9.0.0-rc1020"])

    def test_candidates_are_anchored_at_the_topologically_closest_train_tag(self):
        # The base (find_previous_release_tag's distance pick) anchors the
        # walk: a superseded train that kept tagging sorts newer than the live
        # train's tag but forked farther from HEAD, so the walk starts at the
        # live train's tag.
        tags = [
            release_tag("r9.1.0-rc1025", "stale_train_fork"),
            release_tag("r9.0.0-rc1020", "live_train_fork"),
            release_tag("r9.0.0-rc1019", "live_train_older"),
        ]
        source = under_test.TagSource(
            probe=FakeProbe(),
            tag_lister=lambda: tags,
            at_or_descending_head=lambda commit: False,
            previous_tag_finder=lambda: "r9.0.0-rc1020",
        )
        self.assertEqual(source.candidate_versions(), ["9.0.0-rc1020", "9.0.0-rc1019"])

    def test_no_qualifying_base_resolves_no_candidates(self):
        # A None base means no tag qualifies as the previous release (no tags
        # match the glob, or every one is at/descending HEAD): nothing is
        # probed, however many tags the lister returns.
        source = under_test.TagSource(
            probe=FakeProbe(),
            tag_lister=lambda: [release_tag("r9.1.0-rc1021", "head")],
            at_or_descending_head=lambda commit: True,
            previous_tag_finder=lambda: None,
        )
        self.assertEqual(source.candidate_versions(), [])

    def test_base_missing_from_the_enumerated_candidates_is_a_loud_error(self):
        # The distance computation and the tag listing share the same glob and
        # repo; a base that is absent from the enumerated tags is an invariant
        # violation and fails loudly.
        source = under_test.TagSource(
            probe=FakeProbe(),
            tag_lister=lambda: [release_tag("r9.0.0-rc1020", "c1")],
            at_or_descending_head=lambda commit: False,
            previous_tag_finder=lambda: "r9.1.0-rc1021",
        )
        with self.assertRaises(RuntimeError):
            source.candidate_versions()

    def test_defaults_use_the_shared_previous_release_tag_machinery(self):
        source = under_test.TagSource(probe=FakeProbe())
        self.assertEqual(source._tag_lister.func, previous_release_tag.list_release_tags)
        self.assertEqual(source._tag_lister.keywords["pattern"], under_test.DSC_TAG_GLOB)
        self.assertIs(
            source._at_or_descending_head, previous_release_tag.is_at_or_descending_target
        )
        self.assertEqual(
            source._previous_tag_finder.func, previous_release_tag.find_previous_release_tag
        )
        self.assertEqual(
            source._previous_tag_finder.keywords["tag_pattern"], under_test.DSC_TAG_GLOB
        )

    def test_availability_is_delegated_to_the_probe(self):
        # Never trust a tag alone: publication is decided by the feed-backed
        # probe, not by the tag's existence.
        probe = FakeProbe(published=["9.0.0-rc1020"])
        source = under_test.TagSource(
            probe=probe,
            tag_lister=lambda: [],
            at_or_descending_head=lambda commit: False,
            previous_tag_finder=lambda: None,
        )
        self.assertTrue(source.has_published_binary("9.0.0-rc1020"))
        self.assertEqual(probe.probed, ["9.0.0-rc1020"])


def feed_manifest():
    """A slice of the atlas feed manifest, with the public cloud.json schema."""
    return {
        "versions": [
            {
                "version": "9.1.0-rc1021",
                "date": "09/01/2026",
                "githash": "57a65fd179d5",
                "downloads": [
                    {
                        "arch": "aarch64",
                        "edition": "enterprise",
                        "target": "amazon2023",
                        "archive": {"url": "https://example.com/mongodb.tgz"},
                    }
                ],
            },
            {
                "version": "9.0.0-rc1020",
                "date": "08/19/2026",
                "githash": "72f735fe91bc",
                "downloads": [
                    {
                        "arch": "aarch64",
                        "edition": "enterprise",
                        "target": "amazon2023",
                        "modules": ["atlas"],
                        "archive": {"url": "https://example.com/mongodb.tgz"},
                    },
                    {"arch": "x86_64", "edition": "enterprise", "target": "amazon2023"},
                ],
            },
        ]
    }


class FakeS3Client:
    def __init__(self, body):
        self.body = body
        self.calls = 0

    def get_object(self, Bucket, Key):
        self.calls += 1
        self.requested = (Bucket, Key)
        return {"Body": io.BytesIO(self.body.encode())}


def make_feed_source(manifest=None, **kwargs):
    s3 = FakeS3Client(json.dumps(manifest or feed_manifest()))
    source = under_test.FeedSource(
        DscBinaryQuery(platform="amazon2023", architecture="aarch64", edition="enterprise"),
        s3_client_factory=lambda: s3,
        **kwargs,
    )
    return source, s3


class TestFeedSource(unittest.TestCase):
    def test_probe_only_source_enumerates_nothing(self):
        # The feed is the publication probe: it enumerates no candidates, and
        # enumeration does not touch the manifest (candidate order belongs to
        # git topology).
        source, s3 = make_feed_source()
        self.assertEqual(source.candidate_versions(), [])
        self.assertEqual(s3.calls, 0)

    def test_published_when_the_platform_build_exists(self):
        source, _ = make_feed_source()
        self.assertTrue(source.has_published_binary("9.0.0-rc1020"))

    def test_not_published_when_the_platform_build_is_absent(self):
        manifest = feed_manifest()
        del manifest["versions"][0]["downloads"]
        source, _ = make_feed_source(manifest)
        # rc1021's manifest entry has no downloads in this slice.
        self.assertFalse(source.has_published_binary("9.1.0-rc1021"))

    def test_not_published_when_the_version_is_absent(self):
        source, _ = make_feed_source()
        self.assertFalse(source.has_published_binary("8.3.0-rc1000"))

    def test_not_published_when_the_module_is_missing(self):
        manifest = feed_manifest()
        manifest["versions"][0]["downloads"][0]["modules"] = ["something-else"]
        source, _ = make_feed_source(manifest)
        self.assertFalse(source.has_published_binary("9.1.0-rc1021"))

    def test_published_when_the_module_matches(self):
        manifest = feed_manifest()
        manifest["versions"][0]["downloads"][0]["modules"] = ["atlas"]
        source, _ = make_feed_source(manifest)
        self.assertTrue(source.has_published_binary("9.1.0-rc1021"))

    def test_published_when_the_module_field_is_absent(self):
        # The feed's DSC builds do not declare modules: a download entry
        # matching the requested dimensions qualifies. (DSC-ness comes from
        # the candidate source -- DSC train tags -- not from the entry.)
        source, _ = make_feed_source()
        self.assertTrue(source.has_published_binary("9.1.0-rc1021"))

    def test_manifest_is_fetched_once_across_probes(self):
        source, s3 = make_feed_source()
        source.has_published_binary("9.0.0-rc1020")
        source.has_published_binary("9.1.0-rc1021")
        self.assertEqual(s3.calls, 1)


if __name__ == "__main__":
    unittest.main()
