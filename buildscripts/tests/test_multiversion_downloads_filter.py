"""Unit tests for buildscripts/multiversion_downloads_filter.py.

Resmoke's alias resolution is mocked, so these do not change meaning when last-LTS moves.

The download entries are trimmed copies of a real `multiversion-downloads.json` produced by
`select_multiversion_binaries`: four entries, with `last-lts` already resolved to a concrete
version (`9.0`) rather than appearing as an alias. That resolution is why the filter has to map
aliases before matching, so the fixture keeps it.
"""

import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from buildscripts import multiversion_downloads_filter as under_test


def download_entry(bin_suffix: str) -> dict:
    """Minimal DownloadRequest-shaped entry; only `bin_suffix` is load-bearing for the filter."""
    return {
        "bin_suffix": bin_suffix,
        "discovery_request": {"identifier": bin_suffix},
        "evg_urls_info": {"evg_version_id": f"mongodb_mongo_v{bin_suffix}_deadbeef"},
        "release_urls_info": {"urls": {"binary": f"https://example/{bin_suffix}.tgz"}},
    }


ALL_ENTRIES = [download_entry(v) for v in ("9.0", "8.0.16", "7.0", "8.0")]
ALIAS_TO_VERSION = {"last_lts": "9.0", "last_continuous": "8.3"}


def fake_service():
    service = mock.MagicMock()
    service.get_binary_name_for_version.side_effect = (
        lambda alias, base: f"{base}-{ALIAS_TO_VERSION[alias]}"
    )
    return service


class TestSuffixesFromVersions(unittest.TestCase):
    """Aliases resolve through resmoke; anything else is an exact version."""

    def resolve(self, versions: str) -> set:
        with mock.patch.object(
            under_test.multiversionconstants, "multiversion_service", fake_service()
        ):
            return under_test.suffixes_from_versions(versions)

    def test_single_alias(self):
        self.assertEqual(self.resolve("last_lts"), {"9.0"})

    def test_hyphenated_alias(self):
        # The generator's value may use either separator.
        self.assertEqual(self.resolve("last-lts"), {"9.0"})

    def test_exact_version_passes_through(self):
        # No alias names a specific pinned build, so it is taken literally.
        self.assertEqual(self.resolve("8.0.16"), {"8.0.16"})

    def test_mixed_list(self):
        self.assertEqual(
            self.resolve("last_lts last_continuous 7.0 8.0.16"),
            {"9.0", "8.3", "7.0", "8.0.16"},
        )

    def test_extra_whitespace_is_ignored(self):
        self.assertEqual(self.resolve("  last_lts   7.0  "), {"9.0", "7.0"})

    def test_empty_string(self):
        self.assertEqual(self.resolve(""), set())


class MainTestCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.input = Path(self.tmp.name) / "multiversion-downloads.json"
        self.write(ALL_ENTRIES)

    def write(self, payload):
        self.input.write_text(json.dumps(payload))

    def suffixes(self, path=None):
        return [entry["bin_suffix"] for entry in json.loads(Path(path or self.input).read_text())]

    def run_main(self, versions, argv_extra=()):
        argv = ["prog", "--input", str(self.input), "--versions", versions, *argv_extra]
        with (
            mock.patch.object(under_test.sys, "argv", argv),
            mock.patch.object(
                under_test.multiversionconstants, "multiversion_service", fake_service()
            ),
        ):
            return under_test.main()


class TestMain(MainTestCase):
    def test_keeps_only_the_requested_version(self):
        self.assertEqual(self.run_main("last_lts"), 0)
        self.assertEqual(self.suffixes(), ["9.0"])

    def test_keeps_several(self):
        self.assertEqual(self.run_main("last_lts 8.0.16"), 0)
        self.assertCountEqual(self.suffixes(), ["9.0", "8.0.16"])

    def test_exact_version_only(self):
        self.assertEqual(self.run_main("8.0.16"), 0)
        self.assertEqual(self.suffixes(), ["8.0.16"])

    def test_no_versions_passes_file_through(self):
        # The task generator said nothing: behave exactly as before this script existed.
        self.assertEqual(self.run_main(""), 0)
        self.assertCountEqual(self.suffixes(), ["9.0", "8.0.16", "7.0", "8.0"])

    def test_whitespace_only_versions_passes_file_through(self):
        self.assertEqual(self.run_main("   "), 0)
        self.assertCountEqual(self.suffixes(), ["9.0", "8.0.16", "7.0", "8.0"])

    def test_requesting_everything_is_a_no_op(self):
        self.assertEqual(self.run_main("last_lts 8.0.16 7.0 8.0"), 0)
        self.assertCountEqual(self.suffixes(), ["9.0", "8.0.16", "7.0", "8.0"])

    def test_writes_to_output_leaving_input_untouched(self):
        out = Path(self.tmp.name) / "filtered.json"
        self.assertEqual(self.run_main("last_lts", argv_extra=("--output", str(out))), 0)
        self.assertEqual(self.suffixes(out), ["9.0"])
        self.assertCountEqual(self.suffixes(), ["9.0", "8.0.16", "7.0", "8.0"])

    def test_missing_version_passes_through_by_default(self):
        # Drift between multiversion_selection.sh and what the task asked for must not fail a task
        # while this is being rolled out.
        self.assertEqual(self.run_main("6.0"), 0)
        self.assertCountEqual(self.suffixes(), ["9.0", "8.0.16", "7.0", "8.0"])

    def test_missing_version_fails_under_strict(self):
        self.assertEqual(self.run_main("6.0", argv_extra=("--strict",)), 1)
        self.assertCountEqual(self.suffixes(), ["9.0", "8.0.16", "7.0", "8.0"])

    def test_partially_missing_version_fails_under_strict(self):
        # One resolvable version is not good enough; the absent one is still needed.
        self.assertEqual(self.run_main("last_lts 6.0", argv_extra=("--strict",)), 1)
        self.assertCountEqual(self.suffixes(), ["9.0", "8.0.16", "7.0", "8.0"])

    def test_unresolvable_alias_passes_through(self):
        # An alias resmoke cannot resolve raises; that must not fail the task.
        self.assertEqual(self.run_main("last_patch"), 0)
        self.assertCountEqual(self.suffixes(), ["9.0", "8.0.16", "7.0", "8.0"])

    def test_non_list_payload_is_left_alone(self):
        self.write({"unexpected": "shape"})
        self.assertEqual(self.run_main("last_lts"), 0)
        self.assertEqual(json.loads(self.input.read_text()), {"unexpected": "shape"})

    def test_entry_without_bin_suffix_is_dropped_not_crashed_on(self):
        self.write(ALL_ENTRIES + [{"discovery_request": {"identifier": "mystery"}}])
        self.assertEqual(self.run_main("last_lts"), 0)
        self.assertEqual(self.suffixes(), ["9.0"])

    def test_input_not_read_when_no_versions_given(self):
        # Nothing to do, so the file is not even opened.
        self.input.unlink()
        self.assertEqual(self.run_main(""), 0)


if __name__ == "__main__":
    unittest.main()
