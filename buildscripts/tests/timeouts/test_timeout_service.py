"""Unit tests for timeout_service.py."""
import random
import unittest
from datetime import datetime, timedelta
from unittest.mock import MagicMock

from requests.exceptions import HTTPError
from evergreen import EvergreenApi

import buildscripts.timeouts.timeout_service as under_test
from buildscripts.resmoke_proxy.resmoke_proxy import ResmokeProxyService
from buildscripts.util.teststats import HistoricTaskData

# pylint: disable=missing-docstring,no-self-use,invalid-name,protected-access


def build_mock_service(evg_api=None, resmoke_proxy=None):
    end_date = datetime.now()
    start_date = end_date - timedelta(weeks=2)
    timeout_settings = under_test.TimeoutSettings(
        end_date=end_date,
        start_date=start_date,
    )
    return under_test.TimeoutService(
        evg_api=evg_api if evg_api else MagicMock(spec_set=EvergreenApi),
        resmoke_proxy=resmoke_proxy if resmoke_proxy else MagicMock(spec_set=ResmokeProxyService),
        timeout_settings=timeout_settings)


def tst_stat_mock(file, duration, pass_count):
    return MagicMock(test_file=file, avg_duration_pass=duration, num_pass=pass_count)


class TestGetTimeoutEstimate(unittest.TestCase):
    def test_no_stats_should_return_default_timeout(self):
        mock_evg_api = MagicMock(spec_set=EvergreenApi)
        mock_evg_api.test_stats_by_project.return_value = []
        timeout_service = build_mock_service(evg_api=mock_evg_api)
        timeout_params = under_test.TimeoutParams(
            evg_project="my project",
            build_variant="bv",
            task_name="my task",
            suite_name="my suite",
            is_asan=False,
        )

        timeout = timeout_service.get_timeout_estimate(timeout_params)

        self.assertFalse(timeout.is_specified())

    def test_a_test_with_missing_history_should_cause_a_default_timeout(self):
        mock_evg_api = MagicMock(spec_set=EvergreenApi)
        test_stats = [tst_stat_mock(f"test_{i}.js", 60, 1) for i in range(30)]
        mock_evg_api.test_stats_by_project.return_value = test_stats
        mock_resmoke_proxy = MagicMock(spec_set=ResmokeProxyService)
        mock_resmoke_proxy.list_tests.return_value = ["test_with_no_stats.js"]
        timeout_service = build_mock_service(evg_api=mock_evg_api, resmoke_proxy=mock_resmoke_proxy)
        timeout_params = under_test.TimeoutParams(
            evg_project="my project",
            build_variant="bv",
            task_name="my task",
            suite_name="my suite",
            is_asan=False,
        )

        timeout = timeout_service.get_timeout_estimate(timeout_params)

        self.assertFalse(timeout.is_specified())

    def test_a_test_with_zero_runtime_history_should_cause_a_default_timeout(self):
        mock_evg_api = MagicMock(spec_set=EvergreenApi)
        test_stats = [tst_stat_mock(f"test_{i}.js", 60, 1) for i in range(30)]
        test_stats.append(tst_stat_mock("zero.js", 0.0, 1))
        mock_evg_api.test_stats_by_project.return_value = test_stats
        mock_resmoke_proxy = MagicMock(spec_set=ResmokeProxyService)
        mock_resmoke_proxy.list_tests.return_value = [ts.test_file for ts in test_stats]
        timeout_service = build_mock_service(evg_api=mock_evg_api, resmoke_proxy=mock_resmoke_proxy)
        timeout_params = under_test.TimeoutParams(
            evg_project="my project",
            build_variant="bv",
            task_name="my task",
            suite_name="my suite",
            is_asan=False,
        )

        timeout = timeout_service.get_timeout_estimate(timeout_params)

        self.assertFalse(timeout.is_specified())

    def test_all_tests_with_runtime_history_should_use_custom_timeout(self):
        mock_evg_api = MagicMock(spec_set=EvergreenApi)
        n_tests = 30
        test_runtime = 600
        test_stats = [tst_stat_mock(f"test_{i}.js", test_runtime, 1) for i in range(n_tests)]
        mock_evg_api.test_stats_by_project.return_value = test_stats
        mock_resmoke_proxy = MagicMock(spec_set=ResmokeProxyService)
        mock_resmoke_proxy.list_tests.return_value = [ts.test_file for ts in test_stats]
        timeout_service = build_mock_service(evg_api=mock_evg_api, resmoke_proxy=mock_resmoke_proxy)
        timeout_params = under_test.TimeoutParams(
            evg_project="my project",
            build_variant="bv",
            task_name="my task",
            suite_name="my suite",
            is_asan=False,
        )

        timeout = timeout_service.get_timeout_estimate(timeout_params)

        self.assertTrue(timeout.is_specified())
        self.assertEqual(1860, timeout.calculate_test_timeout(1))
        self.assertEqual(54180, timeout.calculate_task_timeout(1))


class TestGetTaskHookOverhead(unittest.TestCase):
    def test_no_stats_should_return_zero(self):
        timeout_service = build_mock_service()

        overhead = timeout_service.get_task_hook_overhead("suite", is_asan=False, test_count=30,
                                                          historic_stats=None)

        self.assertEqual(0.0, overhead)

    def test_stats_with_no_clean_every_n_should_return_zero(self):
        timeout_service = build_mock_service()
        test_stats = HistoricTaskData.from_stats_list(
            [tst_stat_mock(f"test_{i}.js", 60, 1) for i in range(30)])

        overhead = timeout_service.get_task_hook_overhead("suite", is_asan=False, test_count=30,
                                                          historic_stats=test_stats)

        self.assertEqual(0.0, overhead)

    def test_stats_with_clean_every_n_should_return_overhead(self):
        test_count = 30
        runtime = 25
        timeout_service = build_mock_service()
        test_stat_list = [tst_stat_mock(f"test_{i}.js", 60, 1) for i in range(test_count)]
        test_stat_list.extend([
            tst_stat_mock(f"test_{i}:{under_test.CLEAN_EVERY_N_HOOK}", runtime, 1)
            for i in range(10)
        ])
        random.shuffle(test_stat_list)
        test_stats = HistoricTaskData.from_stats_list(test_stat_list)

        overhead = timeout_service.get_task_hook_overhead(
            "suite", is_asan=True, test_count=test_count, historic_stats=test_stats)

        self.assertEqual(runtime * test_count, overhead)


class TestLookupHistoricStats(unittest.TestCase):
    def test_no_stats_from_evergreen_should_return_none(self):
        mock_evg_api = MagicMock(spec_set=EvergreenApi)
        mock_evg_api.test_stats_by_project.return_value = []
        timeout_service = build_mock_service(evg_api=mock_evg_api)
        timeout_params = under_test.TimeoutParams(
            evg_project="my project",
            build_variant="bv",
            task_name="my task",
            suite_name="my suite",
            is_asan=False,
        )

        stats = timeout_service.lookup_historic_stats(timeout_params)

        self.assertIsNone(stats)

    def test_errors_from_evergreen_should_return_none(self):
        mock_evg_api = MagicMock(spec_set=EvergreenApi)
        mock_evg_api.test_stats_by_project.side_effect = HTTPError("failed to connect")
        timeout_service = build_mock_service(evg_api=mock_evg_api)
        timeout_params = under_test.TimeoutParams(
            evg_project="my project",
            build_variant="bv",
            task_name="my task",
            suite_name="my suite",
            is_asan=False,
        )

        stats = timeout_service.lookup_historic_stats(timeout_params)

        self.assertIsNone(stats)

    def test_stats_from_evergreen_should_return_the_stats(self):
        mock_evg_api = MagicMock(spec_set=EvergreenApi)
        test_stats = [tst_stat_mock(f"test_{i}.js", 60, 1) for i in range(100)]
        mock_evg_api.test_stats_by_project.return_value = test_stats
        timeout_service = build_mock_service(evg_api=mock_evg_api)
        timeout_params = under_test.TimeoutParams(
            evg_project="my project",
            build_variant="bv",
            task_name="my task",
            suite_name="my suite",
            is_asan=False,
        )

        stats = timeout_service.lookup_historic_stats(timeout_params)

        self.assertIsNotNone(stats)
        self.assertEqual(len(test_stats), len(stats.historic_test_results))


class TestGetCleanEveryNCadence(unittest.TestCase):
    def test_clean_every_n_cadence_on_asan(self):
        timeout_service = build_mock_service()

        cadence = timeout_service._get_clean_every_n_cadence("suite", True)

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
        timeout_service = build_mock_service(resmoke_proxy=mock_resmoke_proxy)

        cadence = timeout_service._get_clean_every_n_cadence("suite", False)

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
        timeout_service = build_mock_service(resmoke_proxy=mock_resmoke_proxy)

        cadence = timeout_service._get_clean_every_n_cadence("suite", False)

        self.assertEqual(1, cadence)

    def test_clean_every_n_cadence_no_hook_config(self):
        mock_resmoke_proxy = MagicMock()
        mock_resmoke_proxy.read_suite_config.return_value = {
            "executor": {"hooks": [{
                "class": "hook1",
            }, ]}
        }
        timeout_service = build_mock_service(resmoke_proxy=mock_resmoke_proxy)

        cadence = timeout_service._get_clean_every_n_cadence("suite", False)

        self.assertEqual(1, cadence)
