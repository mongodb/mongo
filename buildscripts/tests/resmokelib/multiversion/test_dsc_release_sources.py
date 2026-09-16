"""Unit tests for the DSC release resolver's discovery routes.

Covers buildscripts/resmokelib/multiversion/dsc_release_sources.py: each
source encodes one external system (the atlas feed's S3 manifest, the
Evergreen API, the git tag listing) and is tested against a fake of that
system. The resolution policy consuming these sources is tested in
test_dsc_release.py.
"""

import io
import json
import unittest

import buildscripts.resmokelib.multiversion.dsc_release_sources as under_test
import buildscripts.resmokelib.multiversion.previous_release_tag as previous_release_tag
from buildscripts.resmokelib.multiversion.dsc_release import resolve
from buildscripts.resmokelib.multiversion.dsc_release_base import (
    ConfigError,
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
        # versionsort); the source only strips the 'r' prefix and filters.
        tags = [release_tag("r9.0.0-rc1020", "commit_b"), release_tag("r9.0.0-rc1013", "commit_a")]
        source = under_test.TagSource(
            probe=FakeProbe(), tag_lister=lambda: tags, at_or_descending_head=lambda commit: False
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
        )
        self.assertEqual(source.candidate_versions(), ["9.0.0-rc1020"])

    def test_defaults_use_the_shared_previous_release_tag_machinery(self):
        source = under_test.TagSource(probe=FakeProbe())
        self.assertEqual(source._tag_lister.func, previous_release_tag.list_release_tags)
        self.assertEqual(source._tag_lister.keywords["pattern"], under_test.DSC_TAG_GLOB)
        self.assertIs(
            source._at_or_descending_head, previous_release_tag.is_at_or_descending_target
        )

    def test_availability_is_delegated_to_the_probe(self):
        # Never trust a tag alone: publication is decided by the feed-backed
        # probe, not by the tag's existence.
        probe = FakeProbe(published=["9.0.0-rc1020"])
        source = under_test.TagSource(
            probe=probe, tag_lister=lambda: [], at_or_descending_head=lambda commit: False
        )
        self.assertTrue(source.has_published_binary("9.0.0-rc1020"))
        self.assertEqual(probe.probed, ["9.0.0-rc1020"])


class FakeBuild:
    def __init__(self, build_variant, status):
        self.build_variant = build_variant
        self.status = status


class FakeEvgApi:
    """Fake evergreen-py client: {version_id: {build_variant: build_status}}."""

    def __init__(self, builds_by_version_id=None, missing_versions=()):
        self.builds_by_version_id = builds_by_version_id or {}
        self.missing_versions = set(missing_versions)
        self.requested_version_ids = []

    def builds_by_version(self, version_id):
        self.requested_version_ids.append(version_id)
        if version_id in self.missing_versions:
            raise RuntimeError(f"version not found: {version_id}")
        variants = self.builds_by_version_id.get(version_id, {})
        return [FakeBuild(variant, status) for variant, status in variants.items()]


class TestEvgVersionSource(unittest.TestCase):
    def make_source(self, api, **kwargs):
        source = under_test.EvgVersionSource(
            DscBinaryQuery(platform="amazon2023", architecture="aarch64", edition="enterprise"),
            api_factory=lambda: api,
            tag_lister=lambda: [release_tag("r9.0.0-rc1020", "commit_rc1020")],
            at_or_descending_head=lambda commit: False,
            **kwargs,
        )
        # Simulate resolve(): candidates first, which maps versions to commits.
        source.candidate_versions()
        return source

    def test_published_when_the_atlas_build_succeeded(self):
        api = FakeEvgApi(
            builds_by_version_id={
                "mongodb_mongo_dsc_release_5_commit_rc1020": {"atlas-amazon2023-arm64": "success"}
            }
        )
        source = self.make_source(api)
        self.assertTrue(source.has_published_binary("9.0.0-rc1020"))

    def test_not_published_when_the_build_failed(self):
        api = FakeEvgApi(
            builds_by_version_id={
                "mongodb_mongo_dsc_release_5_commit_rc1020": {"atlas-amazon2023-arm64": "failed"}
            }
        )
        source = self.make_source(api)
        self.assertFalse(source.has_published_binary("9.0.0-rc1020"))

    def test_not_published_when_the_variant_is_absent(self):
        api = FakeEvgApi(
            builds_by_version_id={
                "mongodb_mongo_dsc_release_5_commit_rc1020": {"unrelated": "success"}
            }
        )
        source = self.make_source(api)
        self.assertFalse(source.has_published_binary("9.0.0-rc1020"))

    def test_unknown_platform_is_a_loud_config_error(self):
        source = under_test.EvgVersionSource(
            DscBinaryQuery(platform="windows", architecture="aarch64", edition="enterprise"),
            api_factory=lambda: FakeEvgApi(),
        )
        source._commits = {"9.0.0-rc1020": "commit_rc1020"}
        with self.assertRaises(ConfigError):
            source.has_published_binary("9.0.0-rc1020")

    def test_falls_through_to_the_next_project(self):
        api = FakeEvgApi(
            builds_by_version_id={
                "mongodb_mongo_dsc_release_6_commit_rc1020": {"atlas-amazon2023-arm64": "success"}
            },
            missing_versions=["mongodb_mongo_dsc_release_5_commit_rc1020"],
        )
        source = self.make_source(api)
        self.assertTrue(source.has_published_binary("9.0.0-rc1020"))
        self.assertEqual(
            api.requested_version_ids,
            [
                "mongodb_mongo_dsc_release_5_commit_rc1020",
                "mongodb_mongo_dsc_release_6_commit_rc1020",
            ],
        )

    def test_version_ids_are_derived_from_the_revision(self):
        api = FakeEvgApi(
            builds_by_version_id={
                "mongodb_mongo_dsc_release_5_commit_rc1020": {"atlas-amazon2023-arm64": "success"}
            }
        )
        source = self.make_source(api)
        source.has_published_binary("9.0.0-rc1020")
        self.assertEqual(api.requested_version_ids, ["mongodb_mongo_dsc_release_5_commit_rc1020"])

    def test_candidates_exclude_tags_at_or_descending_head(self):
        tags = [
            release_tag("r9.0.0-rc2000", "descendant"),
            release_tag("r9.0.0-rc1020", "ancestor"),
        ]
        source = under_test.EvgVersionSource(
            DscBinaryQuery(platform="amazon2023", architecture="aarch64", edition="enterprise"),
            api_factory=lambda: FakeEvgApi(),
            tag_lister=lambda: tags,
            at_or_descending_head=lambda commit: commit == "descendant",
        )
        self.assertEqual(source.candidate_versions(), ["9.0.0-rc1020"])
        self.assertEqual(source._commits, {"9.0.0-rc1020": "ancestor"})

    def test_unavailable_api_steps_down_to_nothing(self):
        # No credentials: the probe raises per candidate; the policy exhausts
        # the source and resolution returns None rather than raising.
        source = under_test.EvgVersionSource(
            DscBinaryQuery(platform="amazon2023", architecture="aarch64", edition="enterprise"),
            api_factory=lambda: (_ for _ in ()).throw(
                RuntimeError("Could not find .evergreen.yml")
            ),
            tag_lister=lambda: [release_tag("r9.0.0-rc1020", "c1")],
            at_or_descending_head=lambda commit: False,
        )
        self.assertIsNone(resolve([source], "9.1.0"))


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
    def test_candidates_are_the_manifest_versions_newest_first(self):
        source, s3 = make_feed_source()
        self.assertEqual(source.candidate_versions(), ["9.1.0-rc1021", "9.0.0-rc1020"])
        self.assertEqual(
            s3.requested,
            (under_test.ATLAS_FEED_BUCKET, under_test.ATLAS_FEED_KEY),
        )

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
        # Public-feed-style entries declare no modules; presence qualifies.
        source, _ = make_feed_source()
        self.assertTrue(source.has_published_binary("9.1.0-rc1021"))

    def test_manifest_is_fetched_once(self):
        source, s3 = make_feed_source()
        source.candidate_versions()
        source.has_published_binary("9.0.0-rc1020")
        self.assertEqual(s3.calls, 1)


if __name__ == "__main__":
    unittest.main()
