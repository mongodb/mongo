"""Unit tests for generate_tss_test_list."""

import contextlib
import io
import os
import pathlib
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

import yaml

sys.path.append(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from bazel.resmoke import generate_tss_test_list as under_test
from buildscripts.resmokelib import config as _config


class TestDiscoverTests(unittest.TestCase):
    def test_flattens_multiple_documents(self):
        # test-discovery emits a multi-document stream when given several suites.
        output = yaml.safe_dump_all(
            [
                {"suite_name": "suite_a", "tests": ["a.js", "b.js"]},
                {"suite_name": "suite_b", "tests": ["c.js"]},
            ]
        )
        with patch.object(under_test.cli, "main", side_effect=lambda argv: print(output)):
            tests = under_test.discover_tests("suite.yml", [], [])
        self.assertEqual(["a.js", "b.js", "c.js"], tests)

    def test_forwards_tag_arguments(self):
        seen = {}

        def fake_main(argv):
            seen["argv"] = argv
            print(yaml.safe_dump({"suite_name": "s", "tests": []}))

        with patch.object(under_test.cli, "main", side_effect=fake_main):
            under_test.discover_tests("suite.yml", ["tags.yml", "more.yml"], ["t1,t2"])

        self.assertEqual(
            [
                "resmoke.py",
                "test-discovery",
                "--suite",
                "suite.yml",
                "--tagFile",
                "tags.yml",
                "--tagFile",
                "more.yml",
                "--excludeWithAnyTags",
                "t1,t2",
            ],
            seen["argv"],
        )

    def test_does_not_enable_test_selection(self):
        # Asking TSS while building the list to ask TSS about would be circular.
        seen = {}

        def fake_main(argv):
            seen["argv"] = argv
            print(yaml.safe_dump({"suite_name": "s", "tests": []}))

        with patch.object(under_test.cli, "main", side_effect=fake_main):
            under_test.discover_tests("suite.yml", [], [])

        self.assertNotIn("--enableEvergreenApiTestSelection", seen["argv"])


MAINLINE = {
    "project": "mongodb-mongo-master",
    "build_variant": "amazon-linux2023-arm64-static-compile",
    "task_name": "resmoke_tests",
    "build_id": (
        "mongodb_mongo_master_amazon_linux2023_arm64_static_compile"
        "_aa312d94e50c9e429d157ab2ae9dcf40f62f1258_26_08_10_13_32_05"
    ),
}

PATCH = {
    "project": "mongodb-mongo-master",
    "build_variant": "promote-sys-perf-build",
    "task_name": "promote_sys_perf_build",
    "build_id": (
        "mongodb_mongo_master_promote_sys_perf_build_patch"
        "_238c5d20ff0586f2ec829e431de5808348d2c63d_6a79dddb29e7b30007ee6a44_26_08_10_14_19_25"
    ),
}


class TestConstructTaskId(unittest.TestCase):
    """Ids are checked against real Evergreen task ids; a wrong id silently misses the cache."""

    def test_mainline_label_keeps_colon_and_converts_slashes(self):
        task_id = under_test.construct_task_id(
            MAINLINE, "//buildscripts/smoke_tests/server_collection_write_path:concurrency"
        )
        self.assertEqual(
            "mongodb_mongo_master_amazon_linux2023_arm64_static_compile"
            "___buildscripts_smoke_tests_server_collection_write_path:concurrency"
            "_aa312d94e50c9e429d157ab2ae9dcf40f62f1258_26_08_10_13_32_05",
            task_id,
        )

    def test_patch_keeps_patch_marker_revision_and_version(self):
        # The task name is spliced in before "_patch_", not after it.
        task_id = under_test.construct_task_id(PATCH, "promote_sys_perf_build")
        self.assertEqual(
            "mongodb_mongo_master_promote_sys_perf_build_promote_sys_perf_build_patch"
            "_238c5d20ff0586f2ec829e431de5808348d2c63d_6a79dddb29e7b30007ee6a44_26_08_10_14_19_25",
            task_id,
        )

    def test_unexpected_build_id_yields_no_id(self):
        volatile = dict(MAINLINE, build_id="something_else_entirely")
        self.assertEqual("", under_test.construct_task_id(volatile, "//pkg:suite"))

    def test_missing_volatile_status_yields_no_id(self):
        self.assertEqual("", under_test.construct_task_id({}, "//pkg:suite"))


class TestTaskIdSelfCheck(unittest.TestCase):
    """The runner's own id is the only ground truth available at build time."""

    def test_recognizes_a_format_it_can_reproduce(self):
        volatile = dict(PATCH, task_id=under_test.construct_task_id(PATCH, PATCH["task_name"]))
        self.assertTrue(under_test.task_id_format_is_understood(volatile))

    def test_rejects_a_format_it_cannot_reproduce(self):
        volatile = dict(PATCH, task_id="mongodb_mongo_master_something_we_did_not_predict")
        self.assertFalse(under_test.task_id_format_is_understood(volatile))

    def test_rejects_missing_task_id(self):
        self.assertFalse(under_test.task_id_format_is_understood(PATCH))


class TestSelectTests(unittest.TestCase):
    """Selection asks Evergreen's REST API; every failure must raise so the caller fails open."""

    @contextlib.contextmanager
    def _evg(self, result=None, side_effect=None):
        api = unittest.mock.MagicMock()
        api.select_tests.return_value = result
        api.select_tests.side_effect = side_effect
        with (
            patch.object(under_test.evergreen_conn, "get_evergreen_api", return_value=api),
            patch.object(under_test, "find_evergreen_config", return_value=None),
        ):
            yield api

    def test_selection_can_only_narrow_never_add(self):
        volatile = dict(MAINLINE, task_id="x", requester="patch_request")
        with self._evg(result={"tests": ["b.js", "not_discovered.js"]}):
            selected = under_test.select_tests(["a.js", "b.js"], "//pkg:suite", volatile, [])
        self.assertEqual(["b.js"], selected)

    def test_sends_the_constructed_task_id_not_the_runner_one(self):
        volatile = dict(MAINLINE, task_id="the_runner_task", requester="patch_request")
        with self._evg(result={"tests": []}) as api:
            under_test.select_tests(["a.js"], "//pkg:suite", volatile, ["Optimistic"])
        kwargs = api.select_tests.call_args[1]
        self.assertNotEqual("the_runner_task", kwargs["task_id"])
        self.assertEqual(under_test.construct_task_id(volatile, "//pkg:suite"), kwargs["task_id"])
        self.assertEqual("//pkg:suite", kwargs["task_name"])
        self.assertEqual(["Optimistic"], kwargs["strategies"])
        self.assertEqual(["a.js"], kwargs["tests"])

    def test_missing_task_id_raises_so_caller_fails_open(self):
        with self._evg(result={"tests": []}):
            with self.assertRaises(RuntimeError):
                under_test.select_tests(["a.js"], "//pkg:suite", {"build_id": "nope"}, [])

    def test_api_error_propagates_so_caller_fails_open(self):
        volatile = dict(MAINLINE, task_id="x", requester="patch_request")
        with self._evg(side_effect=ConnectionError("500 error")):
            with self.assertRaises(ConnectionError):
                under_test.select_tests(["a.js"], "//pkg:suite", volatile, [])

    def test_a_stalled_request_does_not_hold_up_failing_open(self):
        volatile = dict(MAINLINE, task_id="x", requester="patch_request")
        release = threading.Event()
        self.addCleanup(release.set)
        with (
            self._evg(side_effect=lambda **_: release.wait()),
            patch.object(under_test, "_SELECT_TESTS_TIMEOUT", 0.2),
        ):
            started = time.monotonic()
            with self.assertRaises(TimeoutError):
                under_test.select_tests(["a.js"], "//pkg:suite", volatile, [])
        self.assertLess(time.monotonic() - started, 5)

    def test_response_without_tests_raises_rather_than_silently_dropping_tests(self):
        volatile = dict(MAINLINE, task_id="x", requester="patch_request")
        for result in ({"other": []}, ["a.js"], None):
            with self.subTest(result=result), self._evg(result=result):
                with self.assertRaises(RuntimeError):
                    under_test.select_tests(["a.js"], "//pkg:suite", volatile, [])


class TestIsSelectable(unittest.TestCase):
    """Some suites have no test files for selection to reason about."""

    def _write(self, contents):
        with tempfile.NamedTemporaryFile("w", suffix=".yml", delete=False) as suite_file:
            suite_file.write(contents)
        self.addCleanup(os.unlink, suite_file.name)
        return suite_file.name

    def test_a_suite_of_files_is_selectable(self):
        path = self._write(yaml.safe_dump({"test_kind": "js_test", "selector": {"roots": []}}))
        self.assertTrue(under_test.is_selectable(path))

    def test_a_mongos_test_suite_is_not(self):
        # resmoke hands back the selector config as the single test case, not a list of files.
        path = self._write(yaml.safe_dump({"test_kind": "mongos_test", "selector": {"test": ""}}))
        self.assertFalse(under_test.is_selectable(path))

    def test_an_unreadable_config_is_left_to_discovery_to_report(self):
        self.assertTrue(under_test.is_selectable("/nonexistent/suite.yml"))

    def test_valid_yaml_that_is_not_a_mapping_is_left_to_discovery_to_report(self):
        # A crash here would escape main()'s fail-open handling and leave the genrule with no
        # output file at all, which is worse than letting discovery report the bad suite.
        for contents in ("[]\n", "- one\n- two\n", "just a scalar\n", "\n"):
            with self.subTest(contents=contents):
                self.assertTrue(under_test.is_selectable(self._write(contents)))


class TestMongosTestSuiteIsSkipped(unittest.TestCase):
    def test_writes_a_disabled_file_without_running_discovery(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            output = os.path.join(tmpdir, "tests.yml")
            suite = os.path.join(tmpdir, "mongos_test.yml")
            pathlib.Path(suite).write_text(yaml.safe_dump({"test_kind": "mongos_test"}))
            argv = [
                "generate_tss_test_list.py",
                "--suite",
                suite,
                "--label",
                "//src/mongo/s:mongos_test",
                "--output",
                output,
            ]
            with (
                patch.object(sys, "argv", argv),
                patch.object(under_test, "discover_tests") as discover,
                contextlib.redirect_stderr(io.StringIO()),
            ):
                under_test.main()
            discover.assert_not_called()
            with open(output) as written:
                self.assertEqual(
                    {
                        "suite": "//src/mongo/s:mongos_test",
                        "status": "disabled",
                        "tests": [],
                    },
                    yaml.safe_load(written),
                )


class TestReadSelectionSettings(unittest.TestCase):
    """Enable and strategies come from the build settings file written by _tss_settings_file."""

    def _read(self, contents):
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as settings:
            settings.write(contents)
        try:
            return under_test.read_selection_settings(settings.name)
        finally:
            os.unlink(settings.name)

    def test_enabled_with_explicit_strategies(self):
        self.assertEqual(
            (True, ["NotPassing", "Optimistic"]), self._read("true\nNotPassing,Optimistic\n")
        )

    def test_enabled_with_no_strategies_uses_resmokes_default(self):
        # The whole point: the two flows agree on the default without repeating it.
        enabled, strategies = self._read("true\n\n")
        self.assertTrue(enabled)
        self.assertEqual(_config.DEFAULT_EVERGREEN_TEST_SELECTION_STRATEGY, strategies)

    def test_disabled(self):
        self.assertEqual((False, ["ExcludeManuallyQuarantined"]), self._read("false\n\n"))

    def test_missing_file_is_disabled(self):
        self.assertEqual((False, []), under_test.read_selection_settings("/nonexistent.txt"))

    def test_no_path_is_disabled(self):
        self.assertEqual((False, []), under_test.read_selection_settings(""))

    def test_whitespace_is_tolerated(self):
        self.assertEqual((True, ["Optimistic"]), self._read(" TRUE \n  Optimistic , \n"))


class TestSelectionSummaryLine(unittest.TestCase):
    """The summary must not claim narrowing when the selection kept everything."""

    def _run(self, discovered_tests, selected_tests):
        with tempfile.TemporaryDirectory() as tmpdir:
            output = os.path.join(tmpdir, "tests.yml")
            settings = os.path.join(tmpdir, "settings.txt")
            pathlib.Path(settings).write_text("true\nOptimistic\n")
            argv = [
                "generate_tss_test_list.py",
                "--suite",
                "suite.yml",
                "--label",
                "//pkg:suite",
                "--output",
                output,
                "--selection-settings",
                settings,
                "--volatile-status",
                "/nonexistent",
            ]
            stderr = io.StringIO()
            with (
                patch.object(sys, "argv", argv),
                patch.object(under_test, "discover_tests", return_value=discovered_tests),
                patch.object(under_test, "parse_volatile_status", return_value={"task_id": "t"}),
                patch.object(under_test, "task_id_format_is_understood", return_value=True),
                patch.object(under_test, "select_tests", return_value=selected_tests),
                contextlib.redirect_stderr(stderr),
            ):
                under_test.main()
            return stderr.getvalue()


class TestMainFailsOpen(unittest.TestCase):
    """A broken discovery step must never be able to cause tests to be skipped."""

    def _run_main(self, side_effect):
        with tempfile.TemporaryDirectory() as tmpdir:
            output = os.path.join(tmpdir, "tests.yml")
            argv = [
                "generate_tss_test_list.py",
                "--suite",
                "suite.yml",
                "--label",
                "//pkg:suite",
                "--output",
                output,
            ]
            with (
                patch.object(sys, "argv", argv),
                patch.object(under_test.cli, "main", side_effect=side_effect),
            ):
                under_test.main()
            with open(output) as written:
                return yaml.safe_load(written)

    def test_exception_yields_empty_list(self):
        result = self._run_main(RuntimeError("discovery blew up"))
        self.assertEqual({"suite": "//pkg:suite", "status": "failed", "tests": []}, result)

    def test_system_exit_yields_empty_list(self):
        # resmoke's argument parsing exits rather than raising.
        result = self._run_main(SystemExit(2))
        self.assertEqual({"suite": "//pkg:suite", "status": "failed", "tests": []}, result)

    def test_success_writes_discovered_tests(self):
        result = self._run_main(
            lambda argv: print(yaml.safe_dump({"suite_name": "s", "tests": ["a.js"]}))
        )
        self.assertEqual({"suite": "//pkg:suite", "status": "disabled", "tests": ["a.js"]}, result)


if __name__ == "__main__":
    unittest.main()
