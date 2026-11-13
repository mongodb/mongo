"""Unit tests for multiversion_service.py."""

import os
import unittest
from copy import deepcopy
from unittest import TestCase, mock

from packaging.version import Version

import buildscripts.resmokelib.multiversion.multiversion_service as under_test
from buildscripts.resmokelib.config import MultiversionOptions


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

        version_constants = under_test.VersionConstantValues.calculate(
            mongo_version, mongo_releases
        )

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

        version_constants = under_test.VersionConstantValues.calculate(
            mongo_version, mongo_releases
        )

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
    # Resolution is git-only: the newest release tag of the current series before HEAD.
    # Tests mock the git lookup so git is never invoked.

    def _make_service(self, resolver, mongo_version="6.0.5", exclude_prerelease=False):
        mongo_releases = under_test.MongoReleases(
            **{
                "featureCompatibilityVersions": ["5.0", "6.0", "100.0"],
                "longTermSupportReleases": ["5.0"],
                "eolVersions": [],
            }
        )
        patcher = mock.patch.dict(
            os.environ,
            {under_test.LAST_PATCH_EXCLUDE_PRERELEASE_ENV_VAR: "1" if exclude_prerelease else ""},
            clear=False,
        )
        patcher.start()
        self.addCleanup(patcher.stop)
        tag_lookup = mock.patch.object(
            under_test, "find_previous_release_tag", side_effect=resolver
        )
        tag_lookup.start()
        self.addCleanup(tag_lookup.stop)
        version = under_test.MongoVersion(mongo_version=mongo_version)
        return under_test.MultiversionService(
            mongo_version=version,
            mongo_releases=mongo_releases,
            last_patch_behavior=under_test.ASCLastPatchBehavior(version),
        )

    @staticmethod
    def _resolver(tag, capture=None):
        def resolve(_target, tag_pattern, exclude_prerelease=False):
            if capture is not None:
                capture.append((tag_pattern, exclude_prerelease))
            return tag

        return resolve

    # --- general suites: final-only ------------------------------------------------

    def test_exclude_prerelease_resolves_a_final_tag(self):
        service = self._make_service(self._resolver("r6.0.4"), exclude_prerelease=True)
        self.assertEqual(service.get_last_patch_version(), "6.0.4")
        self.assertEqual(service.get_last_patch_fcv(), "6.0")

    def test_exclude_prerelease_skips_when_nothing_precedes_head(self):
        # master's case: the only in-series tag is an alpha, so the filtered walk
        # returns nothing. Also a freshly cut branch whose HEAD is still the tag.
        service = self._make_service(self._resolver(None), exclude_prerelease=True)
        self.assertIsNone(service.get_last_patch_version())

    def test_exclude_prerelease_rejects_a_prerelease_the_resolver_still_returned(self):
        service = self._make_service(self._resolver("r6.0.5-rc1"), exclude_prerelease=True)
        self.assertIsNone(service.get_last_patch_version())

    def test_exclude_prerelease_passes_the_flag_to_the_resolver(self):
        captured = []
        self._make_service(
            self._resolver("r6.0.4", captured), exclude_prerelease=True
        ).get_last_patch_version()
        self.assertEqual(captured, [("r6.0.*", True)])

    def test_first_release_of_a_series_is_valid_once_head_moves_past_it(self):
        # v9.0's case: target is 9.0.0 and r9.0.0 precedes HEAD, so 9.0.0 is exactly
        # what HEAD -- the code that will become 9.0.1 -- should upgrade from.
        service = self._make_service(
            self._resolver("r6.0.0"), mongo_version="6.0.0", exclude_prerelease=True
        )
        self.assertEqual(service.get_last_patch_version(), "6.0.0")

    # --- disagg: permissive (default) -----------------------------------------------

    def test_permissive_accepts_a_prerelease_tag(self):
        service = self._make_service(self._resolver("r6.0.5-rc1010"))
        self.assertEqual(service.get_last_patch_version(), "6.0.5-rc1010")
        self.assertEqual(service.get_last_patch_fcv(), "6.0")

    def test_permissive_passes_the_flag_to_the_resolver(self):
        captured = []
        self._make_service(self._resolver("r6.0.4", captured)).get_last_patch_version()
        self.assertEqual(captured, [("r6.0.*", False)])

    def test_permissive_works_on_a_series_with_no_final_release(self):
        service = self._make_service(self._resolver("r6.0.0-rc4"), mongo_version="6.0.0")
        self.assertEqual(service.get_last_patch_version(), "6.0.0-rc4")

    # --- shared shape / failure handling --------------------------------------------

    def test_resolver_returning_none_leaves_fields_none(self):
        service = self._make_service(self._resolver(None))
        self.assertIsNone(service.get_last_patch_version())
        self.assertIsNone(service.get_last_patch_fcv())

    def test_supported_fixture_fcv_rejects_an_unavailable_old_fcv(self):
        service = self._make_service(self._resolver(None))
        with self.assertRaisesRegex(ValueError, "FCV None.*5.0"):
            service.get_supported_fcv_for_last_patch_bin()

    def test_supported_fixture_fcv_is_last_continuous_for_a_current_series_release(self):
        service = self._make_service(self._resolver("r6.0.4"))
        self.assertEqual(service.get_supported_fcv_for_last_patch_bin(), "5.0")

    def test_resolver_returning_unparseable_tag_falls_back_to_none(self):
        self.assertIsNone(self._make_service(self._resolver("v6.0.4")).get_last_patch_version())

    def test_resolver_returning_tag_missing_patch_component(self):
        self.assertIsNone(self._make_service(self._resolver("r6.0")).get_last_patch_version())

    def test_resolver_returning_invalid_tag(self):
        self.assertIsNone(self._make_service(self._resolver("rfoo")).get_last_patch_version())

    def test_resolver_raising_falls_back_to_none(self):
        def resolver(_target, tag_pattern, exclude_prerelease=False):
            raise RuntimeError("git is unhappy")

        service = self._make_service(resolver)
        self.assertIsNone(service.get_last_patch_version())
        self.assertIsNone(service.get_last_patch_fcv())

    def test_get_version_constants_does_not_invoke_resolver(self):
        def resolver(_target, tag_pattern, exclude_prerelease=False):
            raise AssertionError("resolver should not be called from get_version_constants")

        self._make_service(resolver).get_version_constants()

    def test_get_version_constants_caches_result(self):
        service = self._make_service(self._resolver(None))
        self.assertIs(service.get_version_constants(), service.get_version_constants())

    def test_get_binary_name_for_last_lts(self):
        service = self._make_service(self._resolver(None))
        self.assertEqual(
            service.get_binary_name_for_version(MultiversionOptions.LAST_LTS, "mongod"),
            "mongod-5.0",
        )

    def test_get_binary_name_for_last_continuous(self):
        service = self._make_service(self._resolver(None))
        self.assertEqual(
            service.get_binary_name_for_version(MultiversionOptions.LAST_CONTINUOUS, "mongos"),
            "mongos-5.0",
        )

    def test_get_binary_name_for_last_patch(self):
        service = self._make_service(self._resolver("r6.0.4"), exclude_prerelease=True)
        self.assertEqual(
            service.get_binary_name_for_version(MultiversionOptions.LAST_PATCH, "mongod"),
            "mongod-6.0",
        )

    def test_get_binary_name_for_unknown_option_raises(self):
        service = self._make_service(self._resolver(None))
        with self.assertRaises(ValueError):
            service.get_binary_name_for_version("not_a_real_option", "mongod")

    def test_resolver_called_at_most_once_across_repeated_calls(self):
        captured = []
        service = self._make_service(self._resolver("r6.0.4", captured))
        for _ in range(3):
            service.get_last_patch_version()
            service.get_last_patch_fcv()
        self.assertEqual(captured, [("r6.0.*", False)])

    def test_resolver_called_at_most_once_when_result_is_none(self):
        captured = []
        service = self._make_service(self._resolver(None, captured))
        for _ in range(3):
            service.get_last_patch_version()
            service.get_last_patch_fcv()
        self.assertEqual(captured, [("r6.0.*", False)])


class TestDSCLastPatchBehavior(TestCase):
    def make_service(self, target="42.3.0", download=None, suffix="42.2", lts=None):
        entry = {
            "bin_suffix": suffix,
            "discovery_request": {
                "request_type": "mongo_last_patch_version",
                "identifier": "42.2.0-rc9",
            },
            "evg_urls_info": {"evg_revision": "abcdef"},
            "release_urls_info": {"git_hash": "fedcba"},
        }
        loader = mock.Mock(return_value=[entry] if download is None else download)
        version = under_test.MongoVersion(mongo_version=target)
        releases = under_test.MongoReleases(
            **{
                "featureCompatibilityVersions": ["41.0", "42.2", "42.3", "42.4"],
                "longTermSupportReleases": ["41.0"] if lts is None else lts,
                "eolVersions": [],
            }
        )
        behavior = under_test.DSCLastPatchBehavior(version, releases, loader)
        service = under_test.MultiversionService(version, releases, last_patch_behavior=behavior)
        return service, loader, entry

    def test_supported_fixture_fcv_survives_target_bumps_for_a_last_lts_release(self):
        for target in ("42.3.0", "42.4.0"):
            with self.subTest(target=target):
                service, _, _ = self.make_service(target=target, lts=["41.0", "42.2"])
                self.assertEqual(service.get_supported_fcv_for_last_patch_bin(), "42.2")

    def test_supported_fixture_fcv_is_capped_at_last_continuous(self):
        for series in ("42.3", "42.4"):
            with self.subTest(series=series):
                _, _, entry = self.make_service()
                entry["bin_suffix"] = series
                entry["discovery_request"]["identifier"] = f"{series}.0-rc9"
                service, _, _ = self.make_service(target="42.4.0", download=[entry])
                self.assertEqual(service.get_supported_fcv_for_last_patch_bin(), "42.3")

    def test_supported_fixture_fcv_rejects_an_unsupported_series(self):
        service, _, _ = self.make_service(target="42.4.0")
        with self.assertRaisesRegex(ValueError, "42.2.*42.3"):
            service.get_supported_fcv_for_last_patch_bin()

    def test_selection_and_cache_survive_target_bumps(self):
        for target in ("42.2.0", "42.3.0", "42.4.0"):
            with self.subTest(target=target):
                service, loader, entry = self.make_service(target)
                with mock.patch.object(
                    under_test.ASCLastPatchBehavior,
                    "get_version",
                    side_effect=AssertionError("must not resolve DSC from git"),
                ):
                    for _ in range(2):
                        self.assertEqual(service.get_last_patch_version(), "42.2.0-rc9")
                        self.assertEqual(service.get_last_patch_fcv(), "42.2")
                        self.assertEqual(
                            service.get_binary_name_for_version("last_patch", "mongod"),
                            "mongod-42.2",
                        )
                        self.assertEqual(
                            service.get_binary_name_for_version("last_patch", "mongos"),
                            "mongos-42.2",
                        )
                loader.assert_called_once_with()

    def test_recorded_suffix_override_is_authoritative(self):
        service, _, _ = self.make_service(suffix="dsc-old")
        self.assertEqual(
            service.get_binary_name_for_version("last_patch", "mongod"), "mongod-dsc-old"
        )
        self.assertEqual(service.get_last_patch_fcv(), "42.2")

    def test_recorded_release_override_is_authoritative(self):
        _, _, entry = self.make_service()
        entry["discovery_request"]["identifier"] = "42.2.1-rc7"
        entry["discovery_request"]["bin_suffix"] = "ignored-request-suffix"
        service, _, _ = self.make_service(download=[entry])
        self.assertEqual(service.get_last_patch_version(), "42.2.1-rc7")
        self.assertEqual(service.get_binary_name_for_version("last_patch", "mongod"), "mongod-42.2")

    def test_missing_and_inconsistent_metadata(self):
        _, _, entry = self.make_service()
        cases = [[], {}, [None], [entry, deepcopy(entry)], [entry, {"bin_suffix": "42.2"}]]
        cases.extend([{**entry, "bin_suffix": suffix}] for suffix in (None, "", "../old", "42.3"))
        cases.append(
            [
                {
                    **entry,
                    "discovery_request": {
                        "request_type": "mongo_last_patch_version",
                        "identifier": "invalid",
                    },
                }
            ]
        )
        for manifest in cases:
            with self.subTest(manifest=manifest):
                service, _, _ = self.make_service(download=manifest)
                with self.assertRaises(under_test.LastPatchSelectionError):
                    service.get_binary_name_for_version("last_patch", "mongod")

    def test_other_modes_do_not_load_the_manifest(self):
        service, loader, _ = self.make_service()
        self.assertEqual(service.get_binary_name_for_version("last_lts", "mongod"), "mongod-41.0")
        self.assertEqual(
            service.get_binary_name_for_version("last_continuous", "mongod"), "mongod-42.2"
        )
        loader.assert_not_called()

    def test_ga_eligibility_is_shared_without_loading_dsc_metadata(self):
        service, loader, _ = self.make_service()
        with mock.patch.object(
            under_test, "find_previous_release_tag", return_value="r42.3.0"
        ) as lookup:
            self.assertTrue(service.has_released_patch_version())
        lookup.assert_called_once_with("HEAD", tag_pattern="r42.3.*", exclude_prerelease=True)
        loader.assert_not_called()


class TestHasReleasedPatchVersion(TestCase):
    # Decides whether LAST_PATCH joins `last_versions` for every suite that declares it.
    # Always strict and environment-independent: it runs during task generation, where
    # the general suites' opt-in is not set.

    def _make_service(self, resolver, mongo_version="9.0.1"):
        tag_lookup = mock.patch.object(
            under_test, "find_previous_release_tag", side_effect=resolver
        )
        tag_lookup.start()
        self.addCleanup(tag_lookup.stop)
        version = under_test.MongoVersion(mongo_version=mongo_version)
        return under_test.MultiversionService(
            mongo_version=version,
            mongo_releases=under_test.MongoReleases(
                **{
                    "featureCompatibilityVersions": ["8.0", "9.0", "100.0"],
                    "longTermSupportReleases": ["8.0"],
                    "eolVersions": [],
                }
            ),
            last_patch_behavior=under_test.ASCLastPatchBehavior(version),
        )

    @staticmethod
    def _resolver(tag, capture=None):
        def resolve(_target, tag_pattern, exclude_prerelease=False):
            if capture is not None:
                capture.append((tag_pattern, exclude_prerelease))
            return tag

        return resolve

    def test_offered_when_a_final_tag_precedes_head(self):
        self.assertTrue(self._make_service(self._resolver("r9.0.0")).has_released_patch_version())

    def test_not_offered_when_nothing_precedes_head(self):
        self.assertFalse(self._make_service(self._resolver(None)).has_released_patch_version())

    def test_prerelease_tag_does_not_count(self):
        self.assertFalse(
            self._make_service(self._resolver("r9.0.1-rc0")).has_released_patch_version()
        )

    def test_always_asks_for_final_tags_regardless_of_environment(self):
        captured = []
        with mock.patch.dict(
            os.environ, {under_test.LAST_PATCH_EXCLUDE_PRERELEASE_ENV_VAR: ""}, clear=False
        ):
            self._make_service(self._resolver("r9.0.0", captured)).has_released_patch_version()
        self.assertEqual(captured, [("r9.0.*", True)])

    def test_offered_for_the_first_release_of_a_series(self):
        # target 9.0.0 with r9.0.0 preceding HEAD: valid, HEAD becomes 9.0.1.
        service = self._make_service(self._resolver("r9.0.0"), mongo_version="9.0.0")
        self.assertTrue(service.has_released_patch_version())


class TestGetOldVersions(TestCase):
    def _make_service(self, fcvs, lts, eols):
        version = under_test.MongoVersion(mongo_version=fcvs[-1])
        return under_test.MultiversionService(
            mongo_version=version,
            mongo_releases=under_test.MongoReleases(
                **{
                    "featureCompatibilityVersions": fcvs,
                    "longTermSupportReleases": lts,
                    "eolVersions": eols,
                }
            ),
            last_patch_behavior=under_test.ASCLastPatchBehavior(version),
        )

    def test_last_continuous_distinct_and_not_eol_returns_both(self):
        # latest=6.0, last_continuous=5.3, last_lts=5.0, 5.3 not EOL.
        service = self._make_service(
            fcvs=["4.0", "5.0", "5.1", "5.2", "5.3", "6.0"],
            lts=["4.0", "5.0"],
            eols=[],
        )
        self.assertEqual(
            service.get_last_versions(),
            [MultiversionOptions.LAST_LTS, MultiversionOptions.LAST_CONTINUOUS],
        )

    def test_last_continuous_eol_returns_only_lts(self):
        # latest=6.0, last_continuous=5.3, last_lts=5.0, 5.3 is EOL.
        service = self._make_service(
            fcvs=["4.0", "5.0", "5.1", "5.2", "5.3", "6.0"],
            lts=["4.0", "5.0"],
            eols=["5.3"],
        )
        self.assertEqual(service.get_last_versions(), [MultiversionOptions.LAST_LTS])

    def test_last_continuous_equals_last_lts_returns_only_lts(self):
        # latest=6.0, last_continuous and last_lts both 5.0.
        service = self._make_service(
            fcvs=["4.0", "5.0", "6.0"],
            lts=["4.0", "5.0"],
            eols=[],
        )
        self.assertEqual(service.get_last_versions(), [MultiversionOptions.LAST_LTS])


if __name__ == "__main__":
    unittest.main()
