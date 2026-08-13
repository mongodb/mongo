"""Unit tests for multiversion_service.py."""

import unittest
from unittest import TestCase

from packaging.version import Version

import buildscripts.resmokelib.multiversion.multiversion_service as under_test


class TestTagStr(TestCase):
    def test_require_fcv_tag_should_be_returned(self):
        self.assertEqual(under_test.tag_str(Version("6.0")), "requires_fcv_60")
        self.assertEqual(under_test.tag_str(Version("5.3")), "requires_fcv_53")
        self.assertEqual(under_test.tag_str(Version("31.41")), "requires_fcv_3141")


class TestGetVersion(TestCase):
    def test_version_should_be_extracted(self):
        mongo_version = under_test.MongoVersion(mongo_version="6.0.0-rc5-18-gbcdfaa9035b")

        self.assertEqual(mongo_version.get_version(), Version("6.0"))

    def test_incompatible_version_should_raise_an_exception(self):
        mongo_version = under_test.MongoVersion(mongo_version="not_a_version")

        with self.assertRaises(ValueError):
            mongo_version.get_version()


class TestCalculateFcvConstants(TestCase):
    def test_fcv_constants_should_be_accurate_for_lts_testing(self):
        mongo_version = under_test.MongoVersion(mongo_version="6.0")
        mongo_releases = under_test.MongoReleases(
            **{
                "featureCompatibilityVersions": [
                    "4.0",
                    "4.2",
                    "4.4",
                    "4.7",
                    "4.8",
                    "4.9",
                    "5.0",
                    "5.1",
                    "5.2",
                    "5.3",
                    "6.0",
                    "100.0",
                ],
                "longTermSupportReleases": ["4.0", "4.2", "4.4", "5.0"],
                "eolVersions": [
                    "2.0",
                    "2.2",
                    "2.4",
                    "2.6",
                    "3.0",
                    "3.2",
                    "3.4",
                    "3.6",
                    "4.0",
                    "5.1",
                    "5.2",
                ],
            }
        )

        multiversion_service = under_test.MultiversionService(
            mongo_version=mongo_version,
            mongo_releases=mongo_releases,
        )

        version_constants = multiversion_service.calculate_version_constants()

        self.assertEqual(version_constants.latest, Version("6.0"))
        self.assertEqual(version_constants.last_continuous, Version("5.3"))
        self.assertEqual(version_constants.last_lts, Version("5.0"))
        self.assertEqual(
            version_constants.requires_fcv_tag_list,
            [Version(v) for v in ["5.1", "5.2", "5.3", "6.0"]],
        )
        self.assertEqual(version_constants.requires_fcv_tag_list_continuous, [Version("6.0")])
        self.assertEqual(
            version_constants.fcvs_less_than_latest,
            [
                Version(v)
                for v in ["4.0", "4.2", "4.4", "4.7", "4.8", "4.9", "5.0", "5.1", "5.2", "5.3"]
            ],
        )

    def test_fcv_constants_should_be_accurate_for_future_git_tag(self):
        mongo_version = under_test.MongoVersion(mongo_version="100.0")
        mongo_releases = under_test.MongoReleases(
            **{
                "featureCompatibilityVersions": [
                    "4.0",
                    "4.2",
                    "4.4",
                    "4.7",
                    "4.8",
                    "4.9",
                    "5.0",
                    "5.1",
                    "5.2",
                    "5.3",
                    "6.0",
                    "6.1",
                    "100.0",
                ],
                "longTermSupportReleases": ["4.0", "4.2", "4.4", "5.0", "6.0"],
                "eolVersions": [
                    "2.0",
                    "2.2",
                    "2.4",
                    "2.6",
                    "3.0",
                    "3.2",
                    "3.4",
                    "3.6",
                    "4.0",
                    "5.1",
                    "5.2",
                ],
            }
        )

        multiversion_service = under_test.MultiversionService(
            mongo_version=mongo_version,
            mongo_releases=mongo_releases,
        )

        version_constants = multiversion_service.calculate_version_constants()

        self.assertEqual(version_constants.latest, Version("100.0"))
        self.assertEqual(version_constants.last_continuous, Version("6.1"))
        self.assertEqual(version_constants.last_lts, Version("6.0"))
        self.assertEqual(
            version_constants.requires_fcv_tag_list, [Version(v) for v in ["6.1", "100.0"]]
        )
        self.assertEqual(version_constants.requires_fcv_tag_list_continuous, [Version("100.0")])
        self.assertEqual(
            version_constants.fcvs_less_than_latest,
            [
                Version(v)
                for v in [
                    "4.0",
                    "4.2",
                    "4.4",
                    "4.7",
                    "4.8",
                    "4.9",
                    "5.0",
                    "5.1",
                    "5.2",
                    "5.3",
                    "6.0",
                    "6.1",
                ]
            ],
        )


class TestLastPatchOnMultiversionService(TestCase):
    def _make_service(self, resolver):
        mongo_version = under_test.MongoVersion(mongo_version="6.0")
        mongo_releases = under_test.MongoReleases(
            **{
                "featureCompatibilityVersions": ["5.0", "6.0", "100.0"],
                "longTermSupportReleases": ["5.0"],
                "eolVersions": [],
            }
        )
        return under_test.MultiversionService(
            mongo_version=mongo_version,
            mongo_releases=mongo_releases,
            last_patch_resolver=resolver,
        )

    def test_resolver_returning_tag_populates_fields(self):
        service = self._make_service(lambda _pattern: "r8.3.1-rc1010")
        self.assertEqual(service.get_last_patch_version(), "8.3.1-rc1010")
        self.assertEqual(service.get_last_patch_fcv(), "8.3")

    def test_resolver_returning_plain_release_tag(self):
        service = self._make_service(lambda _pattern: "r8.3.1")
        self.assertEqual(service.get_last_patch_version(), "8.3.1")
        self.assertEqual(service.get_last_patch_fcv(), "8.3")

    def test_resolver_returning_none_leaves_fields_none(self):
        service = self._make_service(lambda _pattern: None)
        self.assertIsNone(service.get_last_patch_version())
        self.assertIsNone(service.get_last_patch_fcv())

    def test_resolver_returning_unparseable_tag_falls_back_to_none(self):
        service = self._make_service(lambda _pattern: "v8.3.1")
        self.assertIsNone(service.get_last_patch_version())

    def test_resolver_returning_alpha_tag(self):
        # The 'alpha' label is preserved verbatim.
        service = self._make_service(lambda _pattern: "r10.0.0-alpha0")
        self.assertEqual(service.get_last_patch_version(), "10.0.0-alpha0")

    def test_resolver_returning_two_digit_minor(self):
        service = self._make_service(lambda _pattern: "r8.12.0")
        self.assertEqual(service.get_last_patch_version(), "8.12.0")

    def test_resolver_returning_tag_missing_patch_component(self):
        service = self._make_service(lambda _pattern: "r8.3")
        self.assertIsNone(service.get_last_patch_version())

    def test_resolver_returning_invalid_tag(self):
        service = self._make_service(lambda _pattern: "rfoo")
        self.assertIsNone(service.get_last_patch_version())

    def test_resolver_raising_falls_back_to_none(self):
        def resolver(_pattern):
            raise RuntimeError("git is unhappy")

        service = self._make_service(resolver)
        self.assertIsNone(service.get_last_patch_version())
        self.assertIsNone(service.get_last_patch_fcv())

    def test_resolver_receives_pattern_bounded_by_latest(self):
        captured = []

        def resolver(tag_pattern):
            captured.append(tag_pattern)
            return None

        self._make_service(resolver).get_last_patch_version()
        self.assertEqual(captured, ["r6.0.*"])

    def test_calculate_version_constants_does_not_invoke_resolver(self):
        def resolver(_pattern):
            raise AssertionError("resolver should not be called from calculate_version_constants")

        # If calculate_version_constants() touched the resolver this would raise.
        self._make_service(resolver).calculate_version_constants()

    def test_resolver_called_at_most_once_across_repeated_calls(self):
        calls = []

        def resolver(tag_pattern):
            calls.append(tag_pattern)
            return "r8.3.1"

        service = self._make_service(resolver)
        for _ in range(3):
            service.get_last_patch_version()
            service.get_last_patch_fcv()
        self.assertEqual(calls, ["r6.0.*"])

    def test_resolver_called_at_most_once_when_result_is_none(self):
        # Cached `None` must not be re-resolved -- the sentinel distinguishes
        # "not computed yet" from "computed, no tag".
        calls = []

        def resolver(tag_pattern):
            calls.append(tag_pattern)
            return None

        service = self._make_service(resolver)
        for _ in range(3):
            service.get_last_patch_version()
            service.get_last_patch_fcv()
        self.assertEqual(calls, ["r6.0.*"])


if __name__ == "__main__":
    unittest.main()
