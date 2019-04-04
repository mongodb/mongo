"""Unit tests for the fetch_test_lifecycle.py script."""
from __future__ import absolute_import

import unittest

import buildscripts.fetch_test_lifecycle as fetch

# pylint: disable=missing-docstring,protected-access


class TestFetchTestLifecycle(unittest.TestCase):
    def test_get_metadata_revision(self):
        metadata_repo = MockMetadataRepository(
            [("metadata_revision_05", "merizo_revision_06"),
             ("metadata_revision_04", "merizo_revision_06"), ("metadata_revision_03",
                                                             "merizo_revision_02"),
             ("metadata_revision_02", "merizo_revision_02"), ("metadata_revision_01", None)])

        merizo_repo = MockMongoRepository([
            "merizo_revision_07", "merizo_revision_06", "merizo_revision_05", "merizo_revision_04",
            "merizo_revision_03", "merizo_revision_02", "merizo_revision_01"
        ])

        self._check_metadata_revision(metadata_repo, merizo_repo, "merizo_revision_07",
                                      "metadata_revision_05")

        self._check_metadata_revision(metadata_repo, merizo_repo, "merizo_revision_06",
                                      "metadata_revision_05")

        self._check_metadata_revision(metadata_repo, merizo_repo, "merizo_revision_05",
                                      "metadata_revision_03")

        self._check_metadata_revision(metadata_repo, merizo_repo, "merizo_revision_04",
                                      "metadata_revision_03")

        self._check_metadata_revision(metadata_repo, merizo_repo, "merizo_revision_03",
                                      "metadata_revision_03")

        self._check_metadata_revision(metadata_repo, merizo_repo, "merizo_revision_02",
                                      "metadata_revision_03")

        self._check_metadata_revision(metadata_repo, merizo_repo, "merizo_revision_01", None)

    def _check_metadata_revision(self, metadata_repo, merizo_repo, merizo_revision,
                                 expected_metadata_revision):
        metadata_revision = fetch._get_metadata_revision(metadata_repo, merizo_repo, "project",
                                                         merizo_revision)
        self.assertEqual(expected_metadata_revision, metadata_revision)


class MockMongoRepository(object):
    def __init__(self, revisions):
        self.revisions = revisions

    def is_ancestor(self, parent, child):
        return (parent in self.revisions and child in self.revisions
                and self.revisions.index(parent) >= self.revisions.index(child))


class MockMetadataRepository(object):
    def __init__(self, references_revisions):
        self.references_revisions = references_revisions

    def list_revisions(self):
        return [r[0] for r in self.references_revisions]

    def get_reference(self, revision, project):  # pylint: disable=unused-argument
        for (metadata_revision, merizo_revision) in self.references_revisions:
            if metadata_revision == revision:
                return merizo_revision
        return None
