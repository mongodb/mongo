"""Unit tests for suite_split.py."""
import unittest
from datetime import datetime
from unittest.mock import MagicMock, patch

import requests

import buildscripts.task_generation.suite_split as under_test
from buildscripts.task_generation.suite_split_strategies import greedy_division, \
    round_robin_fallback
from buildscripts.util.teststats import TestRuntime

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access


def mock_evg_error(mock_evg_api, error_code=requests.codes.SERVICE_UNAVAILABLE):
    response = MagicMock(status_code=error_code)
    mock_evg_api.test_stats_by_project.side_effect = requests.HTTPError(response=response)
    return mock_evg_api


def build_mock_service(evg_api=None, split_config=None, resmoke_proxy=None):

    return under_test.SuiteSplitService(
        evg_api=evg_api if evg_api else MagicMock(),
        resmoke_proxy=resmoke_proxy if resmoke_proxy else MagicMock(),
        config=split_config if split_config else MagicMock(),
        split_strategy=greedy_division,
        fallback_strategy=round_robin_fallback,
    )


def tst_stat_mock(file, duration, pass_count):
    return MagicMock(test_file=file, avg_duration_pass=duration, num_pass=pass_count)


def build_mock_split_config(target_resmoke_time=None, max_sub_suites=None):
    return under_test.SuiteSplitConfig(
        evg_project="project",
        target_resmoke_time=target_resmoke_time if target_resmoke_time else 60,
        max_sub_suites=max_sub_suites if max_sub_suites else 1000,
        max_tests_per_suite=100,
        start_date=datetime.utcnow(),
        end_date=datetime.utcnow(),
    )


def build_mock_split_params(test_filter=None):
    return under_test.SuiteSplitParameters(
        build_variant="build variant",
        task_name="task name",
        suite_name="suite name",
        filename="targetfile",
        test_file_filter=test_filter,
    )


def build_mock_sub_suite(index, test_list):
    return under_test.SubSuite(
        index=index,
        suite_name="suite_name",
        test_list=test_list,
        tests_with_runtime_info=0,
        max_test_runtime=0,
        historic_runtime=0,
        task_overhead=0,
    )


class TestSubSuite(unittest.TestCase):
    def test_tests_with_0_runtime_should_not_override_timeouts(self):
        test_list = [f"test_{i}" for i in range(10)]
        runtime_list = [
            MagicMock(spec_set=TestRuntime, test_name=test, runtime=3.14) for test in test_list
        ]
        runtime_list[3].runtime = 0
        sub_suite = under_test.SubSuite.from_test_list(0, "my_suite", test_list, None, runtime_list)

        assert not sub_suite.should_overwrite_timeout()

    def test_tests_with_full_runtime_history_should_override_timeouts(self):
        test_list = [f"test_{i}" for i in range(10)]
        runtime_list = [
            MagicMock(spec_set=TestRuntime, test_name=test, runtime=3.14) for test in test_list
        ]
        sub_suite = under_test.SubSuite.from_test_list(0, "my_suite", test_list, None, runtime_list)

        assert sub_suite.should_overwrite_timeout()


class TestGeneratedSuite(unittest.TestCase):
    def test_get_test_list_should_run_tests_in_sub_tasks(self):
        n_sub_suites = 3
        n_tests_per_suite = 5
        test_lists = [[f"test_{i * n_tests_per_suite + j}" for j in range(n_tests_per_suite)]
                      for i in range(n_sub_suites)]
        mock_sub_suites = [build_mock_sub_suite(i, test_lists[i]) for i in range(n_sub_suites)]
        mock_suite = under_test.GeneratedSuite(sub_suites=mock_sub_suites,
                                               build_variant="build_variant", task_name="task_name",
                                               suite_name="suite_name", filename="filename")

        all_tests = mock_suite.get_test_list()

        for test_list in test_lists:
            for test in test_list:
                self.assertIn(test, all_tests)

    def test_sub_suite_config_file_should_generate_filename_for_sub_suites(self):
        task_name = "task_name"
        n_sub_suites = 42
        mock_sub_suites = [build_mock_sub_suite(i, []) for i in range(n_sub_suites)]
        mock_suite = under_test.GeneratedSuite(
            sub_suites=mock_sub_suites, build_variant="build_variant", task_name=f"{task_name}_gen",
            suite_name="suite_name", filename="filename")

        self.assertEqual(mock_suite.sub_suite_config_file(34), "task_name_34")
        self.assertEqual(mock_suite.sub_suite_config_file(0), "task_name_00")
        self.assertEqual(mock_suite.sub_suite_config_file(3), "task_name_03")
        self.assertEqual(mock_suite.sub_suite_config_file(None), "task_name_misc")


class TestSplitSuite(unittest.TestCase):
    def test_calculate_suites(self):
        mock_test_stats = [tst_stat_mock(f"test{i}.js", 60, 1) for i in range(100)]
        split_config = build_mock_split_config(target_resmoke_time=10)
        split_params = build_mock_split_params()

        suite_split_service = build_mock_service(split_config=split_config)
        suite_split_service.evg_api.test_stats_by_project.return_value = mock_test_stats
        suite_split_service.resmoke_proxy.list_tests.return_value = [
            stat.test_file for stat in mock_test_stats
        ]
        suite_split_service.resmoke_proxy.read_suite_config.return_value = {}

        with patch("os.path.exists") as exists_mock:
            exists_mock.return_value = True

            suite = suite_split_service.split_suite(split_params)

        # There are 100 tests taking 1 minute, with a target of 10 min we expect 10 suites.
        self.assertEqual(10, len(suite))
        for sub_suite in suite.sub_suites:
            self.assertEqual(10, len(sub_suite.test_list))

    def test_calculate_suites_fallback_on_error(self):
        n_tests = 100
        max_sub_suites = 4
        split_config = build_mock_split_config(max_sub_suites=max_sub_suites)
        split_params = build_mock_split_params()

        suite_split_service = build_mock_service(split_config=split_config)
        mock_evg_error(suite_split_service.evg_api)
        suite_split_service.resmoke_proxy.list_tests.return_value = [
            f"test_{i}.js" for i in range(n_tests)
        ]

        suite = suite_split_service.split_suite(split_params)

        self.assertEqual(max_sub_suites, len(suite))
        for sub_suite in suite.sub_suites:
            self.assertEqual(n_tests / max_sub_suites, len(sub_suite.test_list))

    def test_calculate_suites_uses_fallback_on_no_results(self):
        n_tests = 100
        max_sub_suites = 5
        split_config = build_mock_split_config(max_sub_suites=max_sub_suites)
        split_params = build_mock_split_params()

        suite_split_service = build_mock_service(split_config=split_config)
        suite_split_service.evg_api.test_stats_by_project.return_value = []
        suite_split_service.resmoke_proxy.list_tests.return_value = [
            f"test_{i}.js" for i in range(n_tests)
        ]

        suite = suite_split_service.split_suite(split_params)

        self.assertEqual(max_sub_suites, len(suite))
        for sub_suite in suite.sub_suites:
            self.assertEqual(n_tests / max_sub_suites, len(sub_suite.test_list))

    def test_calculate_suites_uses_fallback_if_only_results_are_filtered(self):
        n_tests = 100
        max_sub_suites = 10
        mock_test_stats = [tst_stat_mock(f"test{i}.js", 60, 1) for i in range(100)]
        split_config = build_mock_split_config(target_resmoke_time=10,
                                               max_sub_suites=max_sub_suites)
        split_params = build_mock_split_params()

        suite_split_service = build_mock_service(split_config=split_config)
        suite_split_service.evg_api.test_stats_by_project.return_value = mock_test_stats
        suite_split_service.resmoke_proxy.list_tests.return_value = [
            f"test_{i}.js" for i in range(n_tests)
        ]
        suite_split_service.resmoke_proxy.read_suite_config.return_value = {}

        with patch("os.path.exists") as exists_mock:
            exists_mock.return_value = False

            suite = suite_split_service.split_suite(split_params)

        # There are 100 tests taking 1 minute, with a target of 10 min we expect 10 suites.
        self.assertEqual(max_sub_suites, len(suite))
        for sub_suite in suite.sub_suites:
            self.assertEqual(n_tests / max_sub_suites, len(sub_suite.test_list))

    def test_calculate_suites_fail_on_unexpected_error(self):
        n_tests = 100
        max_sub_suites = 4
        split_config = build_mock_split_config(max_sub_suites=max_sub_suites)
        split_params = build_mock_split_params()

        suite_split_service = build_mock_service(split_config=split_config)
        mock_evg_error(suite_split_service.evg_api, error_code=requests.codes.INTERNAL_SERVER_ERROR)
        suite_split_service.resmoke_proxy.list_tests.return_value = [
            f"test_{i}.js" for i in range(n_tests)
        ]

        with self.assertRaises(requests.HTTPError):
            suite_split_service.split_suite(split_params)

    def test_calculate_suites_will_filter_specified_tests(self):
        mock_test_stats = [tst_stat_mock(f"test_{i}.js", 60, 1) for i in range(100)]
        split_config = build_mock_split_config(target_resmoke_time=10)
        split_params = build_mock_split_params(
            test_filter=lambda t: t in {"test_1.js", "test_2.js"})

        suite_split_service = build_mock_service(split_config=split_config)
        suite_split_service.evg_api.test_stats_by_project.return_value = mock_test_stats
        suite_split_service.resmoke_proxy.list_tests.return_value = [
            stat.test_file for stat in mock_test_stats
        ]
        suite_split_service.resmoke_proxy.read_suite_config.return_value = {}

        with patch("os.path.exists") as exists_mock:
            exists_mock.return_value = True

            suite = suite_split_service.split_suite(split_params)

        self.assertEqual(1, len(suite))
        for sub_suite in suite.sub_suites:
            self.assertEqual(2, len(sub_suite.test_list))
            self.assertIn("test_1.js", sub_suite.test_list)
            self.assertIn("test_2.js", sub_suite.test_list)


class TestFilterTests(unittest.TestCase):
    def test_filter_missing_files(self):
        tests_runtimes = [
            TestRuntime(test_name="dir1/file1.js", runtime=20.32),
            TestRuntime(test_name="dir2/file2.js", runtime=24.32),
            TestRuntime(test_name="dir1/file3.js", runtime=36.32),
        ]
        mock_params = MagicMock(test_file_filter=None)
        mock_resmoke_proxy = MagicMock()
        mock_resmoke_proxy.list_tests.return_value = [
            runtime.test_name for runtime in tests_runtimes
        ]
        suite_split_service = build_mock_service(resmoke_proxy=mock_resmoke_proxy)

        with patch("os.path.exists") as exists_mock:
            exists_mock.side_effect = [False, True, True]
            filtered_list = suite_split_service.filter_tests(tests_runtimes, mock_params)

        self.assertEqual(2, len(filtered_list))
        self.assertNotIn(tests_runtimes[0], filtered_list)
        self.assertIn(tests_runtimes[2], filtered_list)
        self.assertIn(tests_runtimes[1], filtered_list)

    def test_filter_blacklist_files(self):
        tests_runtimes = [
            TestRuntime(test_name="dir1/file1.js", runtime=20.32),
            TestRuntime(test_name="dir2/file2.js", runtime=24.32),
            TestRuntime(test_name="dir1/file3.js", runtime=36.32),
        ]
        blacklisted_test = tests_runtimes[1][0]
        mock_params = MagicMock(test_file_filter=None)
        mock_resmoke_proxy = MagicMock()
        mock_resmoke_proxy.list_tests.return_value = [
            runtime.test_name for runtime in tests_runtimes if runtime.test_name != blacklisted_test
        ]
        suite_split_service = build_mock_service(resmoke_proxy=mock_resmoke_proxy)

        with patch("os.path.exists") as exists_mock:
            exists_mock.return_value = True

            filtered_list = suite_split_service.filter_tests(tests_runtimes, mock_params)

        self.assertEqual(2, len(filtered_list))
        self.assertNotIn(blacklisted_test, filtered_list)
        self.assertIn(tests_runtimes[2], filtered_list)
        self.assertIn(tests_runtimes[0], filtered_list)

    def test_filter_blacklist_files_for_windows(self):
        tests_runtimes = [
            TestRuntime(test_name="dir1/file1.js", runtime=20.32),
            TestRuntime(test_name="dir2/file2.js", runtime=24.32),
            TestRuntime(test_name="dir1/dir3/file3.js", runtime=36.32),
        ]

        blacklisted_test = tests_runtimes[1][0]

        mock_params = MagicMock(test_file_filter=None)
        mock_resmoke_proxy = MagicMock()
        mock_resmoke_proxy.list_tests.return_value = [
            runtime.test_name.replace("/", "\\") for runtime in tests_runtimes
            if runtime.test_name != blacklisted_test
        ]
        suite_split_service = build_mock_service(resmoke_proxy=mock_resmoke_proxy)

        with patch("os.path.exists") as exists_mock:
            exists_mock.return_value = True

            filtered_list = suite_split_service.filter_tests(tests_runtimes, mock_params)

        self.assertNotIn(blacklisted_test, filtered_list)
        self.assertIn(tests_runtimes[2], filtered_list)
        self.assertIn(tests_runtimes[0], filtered_list)
        self.assertEqual(2, len(filtered_list))


class TestGetCleanEveryNCadence(unittest.TestCase):
    def test_clean_every_n_cadence_on_asan(self):
        split_config = MagicMock(
            san_options="ASAN_OPTIONS=\"detect_leaks=1:check_initialization_order=true\"")
        suite_split_service = build_mock_service(split_config=split_config)

        cadence = suite_split_service._get_clean_every_n_cadence("suite", True)

        self.assertEqual(1, cadence)

    def test_clean_every_n_cadence_from_hook_config(self):
        expected_n = 42
        mock_resmoke_proxy = MagicMock()
        mock_resmoke_proxy.read_suite_config.return_value = {
            "executor": {
                "hooks": [{
                    "class": "hook1",
                }, {
                    "class": under_test.CLEAN_EVERY_N_HOOK,
                    "n": expected_n,
                }]
            }
        }
        suite_split_service = build_mock_service(resmoke_proxy=mock_resmoke_proxy)

        cadence = suite_split_service._get_clean_every_n_cadence("suite", False)

        self.assertEqual(expected_n, cadence)

    def test_clean_every_n_cadence_no_n_in_hook_config(self):
        mock_resmoke_proxy = MagicMock()
        mock_resmoke_proxy.read_suite_config.return_value = {
            "executor": {
                "hooks": [{
                    "class": "hook1",
                }, {
                    "class": under_test.CLEAN_EVERY_N_HOOK,
                }]
            }
        }
        suite_split_service = build_mock_service(resmoke_proxy=mock_resmoke_proxy)

        cadence = suite_split_service._get_clean_every_n_cadence("suite", False)

        self.assertEqual(1, cadence)

    def test_clean_every_n_cadence_no_hook_config(self):
        mock_resmoke_proxy = MagicMock()
        mock_resmoke_proxy.read_suite_config.return_value = {
            "executor": {"hooks": [{
                "class": "hook1",
            }, ]}
        }
        suite_split_service = build_mock_service(resmoke_proxy=mock_resmoke_proxy)

        cadence = suite_split_service._get_clean_every_n_cadence("suite", False)

        self.assertEqual(1, cadence)
