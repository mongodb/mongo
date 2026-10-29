"""Unit tests for the resmokelib.testing.suite module."""

import json
import logging
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import yaml
from mock import MagicMock

from buildscripts.resmokelib.logging import loggers
from buildscripts.resmokelib.testing import suite as under_test
from buildscripts.resmokelib.testing.suite import EqualRuntime
from buildscripts.resmokelib.testing.testcases.interface import TestCase


class TestBypassEnvironmentTransport(unittest.TestCase):
    def test_patch_bypass_reaches_resmoke_as_environment_not_arguments(self):
        root = pathlib.Path(__file__).resolve().parents[4]
        with tempfile.TemporaryDirectory() as tmpdir:
            workdir = pathlib.Path(tmpdir)
            (workdir / "src").mkdir()
            evergreen_dir = workdir / "src/evergreen"
            evergreen_dir.mkdir()
            script = evergreen_dir / "resmoke_tests_execute.sh"
            shutil.copyfile(root / "evergreen/resmoke_tests_execute.sh", script)
            shutil.copyfile(root / "evergreen/prelude.sh", evergreen_dir / "prelude.sh")
            for helper in (
                "prelude_workdir.sh",
                "prelude_python.sh",
                "prelude_db_contrib_tool.sh",
                "prelude_mongo_task_generator.sh",
                "prelude_system_env_variables.sh",
            ):
                (evergreen_dir / helper).write_text("")
            (evergreen_dir / "prelude_venv.sh").write_text(
                "activate_venv() { :; }\nsetup_db_contrib_tool() { :; }\n"
            )
            (workdir / "src/buildscripts").mkdir()
            shutil.copyfile(
                root / "buildscripts/evergreen_expansions2bash.py",
                workdir / "src/buildscripts/evergreen_expansions2bash.py",
            )
            # A failing stub skips the wrapper's successful-run core-file cleanup.
            (workdir / "src/buildscripts/resmoke.py").write_text(
                "import json, os, pathlib, sys\n"
                "pathlib.Path('captured.json').write_text(json.dumps({\n"
                "    'bypass': os.environ.get('TEST_SELECTION_BYPASS_QUARANTINE'),\n"
                "    'args': sys.argv[1:],\n"
                "}))\n"
                "sys.exit(1)\n"
            )
            (workdir / "src/etc").mkdir()
            (workdir / "src/etc/expansions.default.yml").write_text(
                yaml.safe_dump(
                    {
                        "disable_unit_tests": "false",
                        "should_shuffle": "false",
                        "continue_on_failure": "false",
                        "resmoke_jobs": "1",
                        "workdir": str(workdir),
                    }
                )
            )
            marker = workdir / "injected"
            names = [
                "a,b.js",
                " a.js ",
                "a  b.js",
                "a\t\tb.js",
                'a"b.js',
                r"a\b.js",
                "a=b.js",
                f"$(touch {marker})",
                f"`touch {marker}`",
                f"';touch {marker};'",
            ]
            raw = json.dumps(names, indent=2)
            for is_patch, value, inherited, expected in (
                ("true", raw, None, raw),
                ("false", raw, "stale", ""),
                ("true", None, "stale", ""),
            ):
                with self.subTest(is_patch=is_patch, value=value):
                    expansions = {"is_patch": is_patch}
                    if value is not None:
                        expansions["test_selection_bypass_quarantine"] = value
                    expansion_file = workdir / "expansions.yml"
                    expansion_file.write_text(yaml.safe_dump(expansions))
                    env = {**os.environ, "python": sys.executable, "OS": "Darwin"}
                    env.pop("TEST_SELECTION_BYPASS_QUARANTINE", None)
                    if inherited is not None:
                        env["TEST_SELECTION_BYPASS_QUARANTINE"] = inherited
                    captured_file = workdir / "src/captured.json"
                    captured_file.unlink(missing_ok=True)
                    subprocess.run(
                        ["bash", str(script)],
                        cwd=workdir,
                        text=True,
                        capture_output=True,
                        check=True,
                        env=env,
                    )
                    captured = json.loads(captured_file.read_text())
                    self.assertEqual(expected, captured["bypass"])
                    self.assertEqual("run", captured["args"][0])
                    self.assertNotIn("a,b.js", " ".join(captured["args"]))
                    self.assertFalse(marker.exists())


class TestPatchTestSelectionBypass(unittest.TestCase):
    def setUp(self):
        for name, value in (
            ("EVERGREEN_TASK_ID", "task"),
            ("ENABLE_EVERGREEN_API_TEST_SELECTION", True),
            ("EVERGREEN_PATCH_BUILD", True),
            ("TSS_TEST_LIST", None),
            ("SHARD_COUNT", None),
            ("SHARD_INDEX", None),
        ):
            self.enterContext(patch.object(under_test._config, name, value))
        self.enterContext(
            patch.object(loggers, "ROOT_EXECUTOR_LOGGER", logging.getLogger("bypass-test"))
        )
        self.enterContext(patch.dict(os.environ, {}, clear=True))
        self.suite = under_test.Suite("suite_name", {"test_kind": "js_test", "selector": {}})
        self.api = MagicMock()
        self.api.select_tests.return_value = {"tests": ["a.js", "outside.js"]}
        self.client = self.enterContext(
            patch.object(under_test.evergreen_conn, "get_evergreen_api", return_value=self.api)
        )

    def test_restores_only_bypassed_candidates_in_candidate_order(self):
        os.environ["TEST_SELECTION_BYPASS_QUARANTINE"] = '["b  b.js","b  b.js","outside.js"]'
        self.assertEqual(
            (["b  b.js", "a.js"], ["b b.js", "c.js"]),
            self.suite._apply_test_selection(["b  b.js", "a.js", "b b.js", "c.js"]),
        )
        self.assertEqual(
            ["b  b.js", "a.js", "b b.js", "c.js"],
            self.api.select_tests.call_args.kwargs["tests"],
        )
        self.assertNotIn("test_selection_bypass_quarantine", self.api.select_tests.call_args.kwargs)

    def test_invalid_active_bypass_fails_before_client_creation(self):
        os.environ["TEST_SELECTION_BYPASS_QUARANTINE"] = "broken"
        with self.assertRaisesRegex(ValueError, "test_selection_bypass_quarantine"):
            self.suite._apply_test_selection(["a.js"])
        self.client.assert_not_called()

    def test_absent_and_empty_bypass_preserve_selection(self):
        for env in (
            {},
            {"TEST_SELECTION_BYPASS_QUARANTINE": ""},
            {"TEST_SELECTION_BYPASS_QUARANTINE": "[]"},
        ):
            with self.subTest(env=env), patch.dict(os.environ, env, clear=True):
                self.assertEqual(
                    (["a.js"], ["b.js"]), self.suite._apply_test_selection(["b.js", "a.js"])
                )

    def test_disabled_missing_task_and_mainline_ignore_invalid_bypass(self):
        os.environ["TEST_SELECTION_BYPASS_QUARANTINE"] = "broken"
        for name, value in (
            ("ENABLE_EVERGREEN_API_TEST_SELECTION", False),
            ("EVERGREEN_TASK_ID", None),
            ("EVERGREEN_PATCH_BUILD", False),
        ):
            with self.subTest(gate=name), patch.object(under_test._config, name, value):
                self.assertEqual((["a.js"], []), self.suite._apply_test_selection(["a.js"]))
        self.client.assert_not_called()

    def test_api_failures_do_not_select_only_bypassed_tests(self):
        os.environ["TEST_SELECTION_BYPASS_QUARANTINE"] = '["b.js"]'
        for error in (RuntimeError("API unavailable"), under_test.TimeoutError()):
            with self.subTest(error=error):
                self.api.select_tests.side_effect = error
                self.assertEqual(
                    (["a.js", "b.js", "c.js"], []),
                    self.suite._apply_test_selection(["a.js", "b.js", "c.js"]),
                )
        self.client.side_effect = RuntimeError("client unavailable")
        self.assertEqual((["a.js", "b.js"], []), self.suite._apply_test_selection(["a.js", "b.js"]))

    def test_restored_tests_are_partitioned_once_across_real_shards(self):
        os.environ["TEST_SELECTION_BYPASS_QUARANTINE"] = '["b.js","excluded_by_tags.js"]'
        shards = []
        with (
            patch.object(self.suite, "_get_suite_root", return_value=os.getcwd()),
            patch.object(
                under_test._selector,
                "filter_tests",
                side_effect=lambda *args, **kwargs: (
                    ["b.js", "a.js", "c.js"],
                    ["excluded_by_tags.js"],
                ),
            ),
            patch.object(
                under_test._selector, "group_tests", side_effect=lambda kind, cfg, tests: tests
            ),
            patch.object(under_test._config, "SHARD_COUNT", 2),
            patch.object(under_test._config, "HISTORIC_TEST_RUNTIMES", None),
        ):
            for index in range(2):
                with patch.object(under_test._config, "SHARD_INDEX", index):
                    tests, excluded = self.suite._get_tests_for_kind("js_test")
                    shards.append(tests)
                    self.assertEqual(["excluded_by_tags.js", "c.js"], excluded)
        self.assertEqual([["b.js"], ["a.js"]], shards)

    def test_empty_candidates_do_not_parse_or_query(self):
        os.environ["TEST_SELECTION_BYPASS_QUARANTINE"] = "broken"
        self.assertEqual(([], []), self.suite._apply_test_selection([]))
        self.client.assert_not_called()


class TestNumTimesToRepeatTests(unittest.TestCase):
    def setUp(self):
        self.default_repeat_tests = under_test._config.REPEAT_TESTS
        self.suite = under_test.Suite("suite_name", {"test_kind": "js_test"})

    def tearDown(self):
        under_test._config.REPEAT_TESTS = self.default_repeat_tests

    def test_without_num_repeat_tests(self):
        expected_num_repeat_tests = 1
        num_repeat_tests = self.suite.get_num_times_to_repeat_tests()
        self.assertEqual(num_repeat_tests, expected_num_repeat_tests)

    def test_with_num_repeat_tests(self):
        expected_num_repeat_tests = 5
        under_test._config.REPEAT_TESTS = expected_num_repeat_tests
        num_repeat_tests = self.suite.get_num_times_to_repeat_tests()
        self.assertEqual(num_repeat_tests, expected_num_repeat_tests)


class TestNumJobsToStart(unittest.TestCase):
    def setUp(self):
        self.default_repeat_tests = under_test._config.REPEAT_TESTS
        self.default_num_jobs = under_test._config.JOBS

        self.suite = under_test.Suite("suite_name", {"test_kind": "js_test"})
        self.suite._tests = []
        self.num_tests = 5
        for _ in range(self.num_tests):
            self.suite._tests.append(MagicMock(TestCase))

    def tearDown(self):
        under_test._config.REPEAT_TESTS = self.default_repeat_tests
        under_test._config.JOBS = self.default_num_jobs

    def test_num_tests_gte_num_jobs(self):
        num_repeat = 2
        under_test._config.JOBS = 100
        under_test._config.REPEAT_TESTS = num_repeat
        self.assertEqual(self.num_tests * num_repeat, self.suite.get_num_jobs_to_start())


class TestGetTestsForKind(unittest.TestCase):
    def setUp(self):
        loggers.ROOT_EXECUTOR_LOGGER = logging
        self.suite = under_test.Suite("suite_name", {"test_kind": "js_test"})
        self.suite._tests = ["t/test1.js", "t/test2.js", "t/test3.js"]
        self.suite._suite_config = {
            "selector": {"roots": ["testroot/**"]},
            "include_files": ["testroot/test1.js, testroot/test2.js", "testroot/test3.js"],
        }

        # Register the test suite in the suite configuration so _get_suite_root() can find it
        from buildscripts.resmokelib import suitesconfig

        self.original_explicit_suite_roots = suitesconfig.ExplicitSuiteConfig._suite_roots.copy()
        suitesconfig.ExplicitSuiteConfig._suite_roots["suite_name"] = os.getcwd()

        self.default_evergreen_task_id = under_test._config.EVERGREEN_TASK_ID
        self.default_enable_evergreen_api_test_selection = (
            under_test._config.ENABLE_EVERGREEN_API_TEST_SELECTION
        )
        self.default_evergreen_test_selection_strategy = (
            under_test._config.EVERGREEN_TEST_SELECTION_STRATEGY
        )
        self.default_evergreen_project_name = under_test._config.EVERGREEN_PROJECT_NAME
        self.default_evergreen_variant_name = under_test._config.EVERGREEN_VARIANT_NAME
        self.default_evergreen_requester = under_test._config.EVERGREEN_REQUESTER
        self.default_evergreen_patch_build = under_test._config.EVERGREEN_PATCH_BUILD
        self.default_evergreen_task_name = under_test._config.EVERGREEN_TASK_NAME
        self.default_evergreen_display_task_name = under_test._config.EVERGREEN_DISPLAY_TASK_NAME
        self.default_selector = under_test._selector
        self.default_evergreen_conn = under_test.evergreen_conn

    def tearDown(self):
        # Restore original suite configuration
        from buildscripts.resmokelib import suitesconfig

        suitesconfig.ExplicitSuiteConfig._suite_roots = self.original_explicit_suite_roots

        under_test._config.ENABLE_EVERGREEN_API_TEST_SELECTION = (
            self.default_enable_evergreen_api_test_selection
        )
        under_test._config.EVERGREEN_PROJECT_NAME = self.default_evergreen_project_name
        under_test._config.EVERGREEN_VARIANT_NAME = self.default_evergreen_variant_name
        under_test._config.EVERGREEN_REQUESTER = self.default_evergreen_requester
        under_test._config.EVERGREEN_TASK_ID = self.default_evergreen_task_id
        under_test._config.EVERGREEN_PATCH_BUILD = self.default_evergreen_patch_build
        under_test._config.EVERGREEN_TASK_NAME = self.default_evergreen_task_name
        under_test._config.EVERGREEN_DISPLAY_TASK_NAME = self.default_evergreen_display_task_name
        under_test._config.EVERGREEN_TEST_SELECTION_STRATEGY = (
            self.default_evergreen_test_selection_strategy
        )
        under_test._selector = self.default_selector
        under_test.evergreen_conn = self.default_evergreen_conn

    def test_simple(self):
        self.assertFalse(under_test._config.ENABLE_EVERGREEN_API_TEST_SELECTION)

        tests, excluded = self.suite._get_tests_for_kind("js_test")
        self.assertEqual(tests, ["testroot"])
        self.assertEqual(excluded, [])

    def test_with_test_selection_strategy(self):
        # mock selector
        mock_selector = MagicMock()
        mock_selector.filter_tests.return_value = (["test1", "test2"], ["excluded_test"])
        mock_selector.group_tests.return_value = ["test1", "test2"]

        # Mock _config values
        under_test._config.ENABLE_EVERGREEN_API_TEST_SELECTION = True
        under_test._config.EVERGREEN_PROJECT_NAME = "project_name"
        under_test._config.EVERGREEN_VARIANT_NAME = "variant_name"
        under_test._config.EVERGREEN_REQUESTER = "requester"
        under_test._config.EVERGREEN_TASK_ID = "task_id"
        under_test._config.EVERGREEN_TASK_NAME = "task_name"
        under_test._config.EVERGREEN_TEST_SELECTION_STRATEGY = "strategy"
        under_test._config.EVERGREEN_PATCH_BUILD = True

        # Mock Evergreen API
        mock_evg_api = MagicMock()
        mock_evg_api.select_tests.return_value = {"tests": ["test1", "test2"]}
        under_test.evergreen_conn = MagicMock()  # Mock evergreen_conn object
        under_test.evergreen_conn.get_evergreen_api.return_value = mock_evg_api

        # Replace _selector in under_test with the mock
        under_test._selector = mock_selector

        # Call `_get_tests_for_kind` and verify its behavior
        tests, excluded = self.suite._get_tests_for_kind("js_test")

        # Assert the expected results
        self.assertEqual(tests, ["test1", "test2"])
        self.assertEqual(excluded, ["excluded_test"])

        mock_selector.filter_tests.assert_called_once_with(
            "js_test", self.suite.get_selector_config(), suite_root=os.getcwd()
        )

        if under_test._config.ENABLE_EVERGREEN_API_TEST_SELECTION:
            mock_evg_api.select_tests.assert_called_once()
            mock_evg_api.select_tests.assert_called_once_with(
                project_id="project_name",
                build_variant="variant_name",
                requester="requester",
                task_id="task_id",
                task_name="task_name",
                tests=["test1", "test2"],
                strategies="strategy",
            )

    def test_test_selection_reports_display_task_name(self):
        """TSS must be called with the display task name, not the execution task name.

        Which execution task (shard) a test lands in is not stable, but the display task those
        shards roll up to always is, so test history has to be keyed on the display task.
        """
        mock_selector = MagicMock()
        mock_selector.filter_tests.return_value = (["test1"], [])
        mock_selector.group_tests.side_effect = lambda _kind, _cfg, tests: tests

        under_test._config.ENABLE_EVERGREEN_API_TEST_SELECTION = True
        under_test._config.EVERGREEN_PROJECT_NAME = "project_name"
        under_test._config.EVERGREEN_VARIANT_NAME = "variant_name"
        under_test._config.EVERGREEN_REQUESTER = "requester"
        under_test._config.EVERGREEN_TASK_ID = "task_id"
        under_test._config.EVERGREEN_TASK_NAME = "multiversion_auth_0_variant_name"
        under_test._config.EVERGREEN_TEST_SELECTION_STRATEGY = "strategy"
        under_test._config.EVERGREEN_PATCH_BUILD = True

        mock_evg_api = MagicMock()
        mock_evg_api.select_tests.return_value = {"tests": ["test1"]}
        under_test.evergreen_conn = MagicMock()
        under_test.evergreen_conn.get_evergreen_api.return_value = mock_evg_api
        under_test._selector = mock_selector

        under_test._config.EVERGREEN_DISPLAY_TASK_NAME = "multiversion_auth"

        self.suite._get_tests_for_kind("js_test")

        mock_evg_api.select_tests.assert_called_once_with(
            project_id="project_name",
            build_variant="variant_name",
            requester="requester",
            task_id="task_id",
            task_name="multiversion_auth",
            tests=["test1"],
            strategies="strategy",
        )

    def test_test_selection_falls_back_to_execution_task_name(self):
        """Without a display task name, send the task's own name."""
        under_test._config.EVERGREEN_TASK_NAME = "some_task"
        under_test._config.EVERGREEN_DISPLAY_TASK_NAME = None
        self.assertEqual(under_test.tss_task_name(), "some_task")

    def test_tss_cannot_add_tests_outside_tag_filtered_list(self):
        """TSS returning tests not in the tag-filtered list must not cause them to run.

        When TestRunnerEvg splits a suite into "resource intensive" and "not resource intensive"
        halves, each half is independently tag-filtered before TSS is called. If TSS returns
        tests that were filtered out by tags (e.g. returning a resource_intensive test for the
        "not resource intensive" suite), the current code accepts them via
        `tests = evergreen_filtered_tests`, bypassing the tag filter. That test then runs in
        both the resource-intensive suite (correct) and the normal suite (incorrect).
        """
        mock_selector = MagicMock()
        # Simulate Suite B (not resource_intensive): tag filter produced only test_normal.js.
        mock_selector.filter_tests.return_value = (["test_normal.js"], [])
        # group_tests passes tests through unchanged.
        mock_selector.group_tests.side_effect = lambda _kind, _cfg, tests: tests

        under_test._config.ENABLE_EVERGREEN_API_TEST_SELECTION = True
        under_test._config.EVERGREEN_PROJECT_NAME = "project"
        under_test._config.EVERGREEN_VARIANT_NAME = "variant"
        under_test._config.EVERGREEN_REQUESTER = "requester"
        under_test._config.EVERGREEN_TASK_ID = "task_id"
        under_test._config.EVERGREEN_TASK_NAME = "task_name"
        under_test._config.EVERGREEN_TEST_SELECTION_STRATEGY = ["NotFailing"]
        under_test._config.EVERGREEN_PATCH_BUILD = True

        # TSS returns test_ri.js in addition to test_normal.js.  test_ri.js was not in the
        # tag-filtered list — it belongs to the resource_intensive suite, not this one.
        mock_evg_api = MagicMock()
        mock_evg_api.select_tests.return_value = {
            "tests": ["test_normal.js", "test_ri.js"],
        }
        under_test.evergreen_conn = MagicMock()
        under_test.evergreen_conn.get_evergreen_api.return_value = mock_evg_api
        under_test._selector = mock_selector

        tests, excluded = self.suite._get_tests_for_kind("js_test")

        # test_ri.js was not in the tag-filtered input; TSS must not be able to add it.
        self.assertNotIn("test_ri.js", tests)
        self.assertIn("test_normal.js", tests)

    def test_sharding(self):
        tests = ["1.js", "2.js", "3.js"]
        shard_count = 2
        shard1 = self.suite.filter_tests_for_shard(tests, shard_count, 0)
        shard2 = self.suite.filter_tests_for_shard(tests, shard_count, 1)
        self.assertEqual(set(shard1), set(["1.js", "3.js"]))
        self.assertEqual(shard2, ["2.js"])

        actual = shard1 + shard2
        actual.sort()
        self.assertEqual(actual, tests)

    def test_runtime_sharding(self):
        tests = ["1.js", "2.js", "3.js", "4.js"]
        runtimes = [
            {
                "test_name": "1.js",
                "avg_duration_pass": 5,
            },
            {
                "test_name": "2.js",
                "avg_duration_pass": 1,
            },
            {
                "test_name": "3.js",
                "avg_duration_pass": 2,
            },
            {
                "test_name": "4.js",
                "avg_duration_pass": 2,
            },
        ]
        shard_count = 2

        strategy = EqualRuntime(runtimes=runtimes)
        shard1 = strategy.get_tests_for_shard(tests, shard_count, 0)
        shard2 = strategy.get_tests_for_shard(tests, shard_count, 1)

        self.assertEqual(
            shard1,
            [
                "1.js",
            ],
        )
        self.assertEqual(set(shard2), set(["2.js", "3.js", "4.js"]))

        actual = shard1 + shard2
        actual.sort()
        self.assertEqual(actual, tests)

    def test_runtime_sharding_no_history(self):
        tests = ["1.js", "2.js", "3.js", "4.js"]
        runtimes = []
        shard_count = 2

        strategy = EqualRuntime(runtimes=runtimes)
        shard1 = strategy.get_tests_for_shard(tests, shard_count, 0)
        shard2 = strategy.get_tests_for_shard(tests, shard_count, 1)

        self.assertEqual(set(shard1), set(["1.js", "3.js"]))
        self.assertEqual(set(shard2), set(["2.js", "4.js"]))

    def test_runtime_sharding_partial_history(self):
        tests = ["1.js", "2.js", "3.js", "4.js"]
        runtimes = [
            {
                "test_name": "1.js",
                "avg_duration_pass": 1,
            },
        ]
        shard_count = 2

        strategy = EqualRuntime(runtimes=runtimes)
        shard1 = strategy.get_tests_for_shard(tests, shard_count, 0)
        shard2 = strategy.get_tests_for_shard(tests, shard_count, 1)

        self.assertEqual(set(shard1), set(["1.js", "3.js"]))
        self.assertEqual(set(shard2), set(["2.js", "4.js"]))


class TestApplyPregeneratedTestSelection(unittest.TestCase):
    """Selection is applied from a build-time file; every failure mode must keep all tests."""

    def setUp(self):
        self.suite = under_test.Suite("suite_name", {"test_kind": "js_test"})
        self.tests = ["a.js", "b.js", "c.js"]
        self.original_tss_test_list = under_test._config.TSS_TEST_LIST
        if under_test.loggers.ROOT_EXECUTOR_LOGGER is None:
            under_test.loggers.ROOT_EXECUTOR_LOGGER = logging.getLogger("executor")

    def tearDown(self):
        under_test._config.TSS_TEST_LIST = self.original_tss_test_list

    def _apply(self, contents):
        with tempfile.NamedTemporaryFile("w", suffix=".yml", delete=False) as test_list_file:
            if contents is not None:
                test_list_file.write(contents)
        under_test._config.TSS_TEST_LIST = test_list_file.name
        try:
            return self.suite._apply_pregenerated_test_selection(self.tests)
        finally:
            os.unlink(test_list_file.name)

    def test_applies_a_successful_selection(self):
        keep, excluded = self._apply(
            yaml.safe_dump({"status": "selected", "tests": ["a.js", "c.js"]})
        )
        self.assertEqual(["a.js", "c.js"], keep)
        self.assertEqual(["b.js"], excluded)
        self.assertIsNone(self.suite.tss_selection_error)

    def test_selection_cannot_add_tests_outside_the_suite(self):
        keep, excluded = self._apply(
            yaml.safe_dump({"status": "selected", "tests": ["a.js", "not_in_suite.js"]})
        )
        self.assertEqual(["a.js"], keep)
        self.assertEqual(["b.js", "c.js"], excluded)

    def test_a_selection_of_nothing_is_honoured(self):
        # An empty list from a successful selection genuinely means every test was excluded.
        keep, excluded = self._apply(yaml.safe_dump({"status": "selected", "tests": []}))
        self.assertEqual([], keep)
        self.assertEqual(self.tests, excluded)
        self.assertIsNone(self.suite.tss_selection_error)

    def test_selection_not_asked_for_runs_everything_without_an_error(self):
        keep, excluded = self._apply(yaml.safe_dump({"status": "disabled", "tests": self.tests}))
        self.assertEqual(self.tests, keep)
        self.assertEqual([], excluded)
        self.assertIsNone(self.suite.tss_selection_error)

    def test_a_failed_selection_runs_everything_and_fails_the_run(self):
        keep, excluded = self._apply(yaml.safe_dump({"status": "failed", "tests": []}))
        self.assertEqual(self.tests, keep)
        self.assertEqual([], excluded)
        self.assertIsNotNone(self.suite.tss_selection_error)

    def test_missing_file_runs_everything_and_fails_the_run(self):
        under_test._config.TSS_TEST_LIST = "/nonexistent/test_list.yml"
        keep, _ = self.suite._apply_pregenerated_test_selection(self.tests)
        self.assertEqual(self.tests, keep)
        self.assertIsNotNone(self.suite.tss_selection_error)

    def test_unparseable_file_runs_everything_and_fails_the_run(self):
        keep, _ = self._apply("{not: valid: yaml: at all")
        self.assertEqual(self.tests, keep)
        self.assertIsNotNone(self.suite.tss_selection_error)

    def test_file_without_a_status_runs_everything_without_an_error(self):
        # An older or hand-written file with no status is treated as "not asked for".
        keep, _ = self._apply(yaml.safe_dump({"tests": []}))
        self.assertEqual(self.tests, keep)
        self.assertIsNone(self.suite.tss_selection_error)


if __name__ == "__main__":
    unittest.main()
