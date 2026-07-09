#!/usr/bin/env python3
"""Unit tests for the resmokelib.testing.hooks.combine_benchmark_results module."""

import datetime
import logging
import subprocess
import unittest

import mock
import requests
from github import GithubException

import buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results as cbr
from buildscripts.resmokelib.errors import ServerFailure
from buildscripts.util.cedar_report import CedarMetric

_BM_CONTEXT = {
    "date": "2018/01/30-18:40:25",
    "executable": "./path/to/exe",
    "num_cpus": 40,
    "mhz_per_cpu": 4999,
    "cpu_scaling_enabled": False,
    "library_build_type": "debug",
    "caches": [],
}

_BM_REPORT_1 = {
    "name": "BM_Name1/arg1/arg with space",
    "run_type": "iteration",
    "repetition_index": 0,
    "threads": 1,
    "iterations": 1000,
    "real_time": 1202,
    "cpu_time": 1303,
    "bytes_per_second": 1404,
    "items_per_second": 1505,
    "custom_counter_1": 1606,
}

_BM_REPORT_2 = {
    "name": "BM_Name1/arg1/arg with space",
    "run_type": "iteration",
    "repetition_index": 1,
    "threads": 1,
    "iterations": 1000,
    "real_time": 1204,
    "cpu_time": 1305,
    "bytes_per_second": 1406,
    "items_per_second": 1507,
    "custom_counter_1": 1608,
}

_BM_REPORT_WITH_INSTRUCTIONS_1 = {
    "name": "BM_Name1/arg1/arg with space",
    "run_type": "iteration",
    "repetition_index": 0,
    "threads": 1,
    "iterations": 1000,
    "real_time": 1204,
    "cpu_time": 1305,
    "bytes_per_second": 1406,
    "items_per_second": 1507,
    "custom_counter_1": 1608,
    "instructions_per_iteration": 101,
}

_BM_REPORT_WITH_INSTRUCTIONS_2 = {
    "name": "BM_Name1/arg1/arg with space",
    "run_type": "iteration",
    "repetition_index": 0,
    "threads": 2,
    "iterations": 1000,
    "real_time": 1202,
    "cpu_time": 1303,
    "bytes_per_second": 1404,
    "items_per_second": 1505,
    "custom_counter_1": 1606,
    "instructions_per_iteration": 100,
}

_BM_REPORT_WITH_INSTRUCTIONS_MEAN = {
    "name": "BM_Name1/arg1/arg with space_mean",
    "run_type": "aggregate",
    "repetition_index": 0,
    "threads": 2,
    "iterations": 1000,
    "real_time": 1202,
    "cpu_time": 1303,
    "bytes_per_second": 1404,
    "items_per_second": 1505,
    "custom_counter_1": 1606,
    "instructions_per_iteration": 100,
    "aggregate_name": "mean",
}

_BM_REPORT_WITH_CYCLES_1 = {
    "name": "BM_Name1/arg1/arg with space",
    "run_type": "iteration",
    "repetition_index": 0,
    "threads": 1,
    "iterations": 1000,
    "real_time": 1204,
    "cpu_time": 1305,
    "bytes_per_second": 1406,
    "items_per_second": 1507,
    "custom_counter_1": 1608,
    "cycles_per_iteration": 101,
}

_BM_REPORT_WITH_CYCLES_2 = {
    "name": "BM_Name1/arg1/arg with space",
    "run_type": "iteration",
    "repetition_index": 0,
    "threads": 2,
    "iterations": 1000,
    "real_time": 1202,
    "cpu_time": 1303,
    "bytes_per_second": 1404,
    "items_per_second": 1505,
    "custom_counter_1": 1606,
    "cycles_per_iteration": 100,
}

_BM_REPORT_WITH_CYCLES_MEAN = {
    "name": "BM_Name1/arg1/arg with space_mean",
    "run_type": "aggregate",
    "repetition_index": 0,
    "threads": 2,
    "iterations": 1000,
    "real_time": 1202,
    "cpu_time": 1303,
    "bytes_per_second": 1404,
    "items_per_second": 1505,
    "custom_counter_1": 1606,
    "cycles_per_iteration": 100,
    "aggregate_name": "mean",
}

_BM_MEAN_REPORT = {
    "name": "BM_Name1/arg1/arg with space_mean",
    "run_type": "aggregate",
    "threads": 1,
    "aggregate_name": "mean",
    "iterations": 1000,
    "real_time": 1200,
    "cpu_time": 1300,
    "bytes_per_second": 1400,
    "items_per_second": 1500,
    "custom_counter_1": 1600,
}

_BM_MULTITHREAD_REPORT = {
    "name": "BM_Name2/threads:10",
    "run_type": "iteration",
    "repetition_index": 0,
    "threads": 10,
    "iterations": 100,
    "real_time": 202,
    "cpu_time": 303,
    "bytes_per_second": 404,
    "items_per_second": 505,
    "custom_counter_1": 606,
}

_BM_MULTITHREAD_MEDIAN_REPORT = {
    "name": "BM_Name2/threads:10_median",
    "run_type": "aggregate",
    "threads": 10,
    "aggregate_name": "median",
    "iterations": 100,
    "real_time": 200,
    "cpu_time": 300,
    "bytes_per_second": 400,
    "items_per_second": 500,
    "custom_counter_1": 600,
}

_BM_FULL_REPORT = {
    "context": _BM_CONTEXT,
    "benchmarks": [
        _BM_REPORT_1,
        _BM_REPORT_2,
        _BM_MEAN_REPORT,
        _BM_MULTITHREAD_REPORT,
        _BM_MULTITHREAD_MEDIAN_REPORT,
    ],
}

_BM_FULL_REPORT_WITH_DUPS = {
    "context": _BM_CONTEXT,
    "benchmarks": [
        _BM_REPORT_1,
        _BM_REPORT_1,
        _BM_REPORT_2,
        _BM_MEAN_REPORT,
        _BM_MULTITHREAD_REPORT,
        _BM_MULTITHREAD_MEDIAN_REPORT,
    ],
}

# 12/31/2999 @ 11:59pm (UTC)
_START_TIME = 32503679999

# 01/01/3000 @ 12:00am (UTC)
_END_TIME = 32503680000


class GenerateAndCheckPerfResultsFixture(unittest.TestCase):
    # Mock the hook's parent class because we're testing only functionality of this hook and
    # not anything related to or inherit from the parent class.
    @mock.patch("buildscripts.resmokelib.testing.hooks.interface.Hook", autospec=True)
    def setUp(self, MockHook):
        self.bm_threads_report = cbr._BenchmarkThreadsReport(_BM_CONTEXT)

        self.cbr_hook = cbr.GenerateAndCheckPerfResults(logging.getLogger("hook_logger"), None)
        self.cbr_hook.logger = logging.getLogger("hook_logger")
        self.cbr_hook.fixture = None

        self.cbr_hook.create_time = datetime.datetime.fromtimestamp(
            _START_TIME, tz=datetime.timezone.utc
        ).replace(tzinfo=None)
        self.cbr_hook.end_time = datetime.datetime.fromtimestamp(
            _END_TIME, tz=datetime.timezone.utc
        ).replace(tzinfo=None)
        self.cbr_hook._parse_report(_BM_FULL_REPORT)


class TestGenerateAndCheckPerfResults(GenerateAndCheckPerfResultsFixture):
    def test_generate_cedar_report(self):
        report = self.cbr_hook._generate_cedar_report(self.cbr_hook._parse_report(_BM_FULL_REPORT))

        self.assertEqual(len(report), 2)
        self.assertEqual(report[0].thread_level, 1)
        self.assertEqual(len(report[0].metrics), 3)
        self.assertEqual(report[1].thread_level, 10)
        self.assertEqual(len(report[1].metrics), 2)

    def test_generate_cedar_report_with_dup_metric_names(self):
        # After we parse the same report twice we have duplicated metrics
        report = self.cbr_hook._parse_report(_BM_FULL_REPORT_WITH_DUPS)

        with self.assertLogs("hook_logger", level="ERROR") as cm:
            cedar_report = self.cbr_hook._generate_cedar_report(report)

        self.assertTrue(
            any(_BM_REPORT_1["name"] in msg for msg in cm.output),
            f"Expected error about duplicated metric names in {cm.output}",
        )
        # The duplicated benchmark should be skipped, but other benchmarks should still be present.
        self.assertEqual(len(cedar_report), 1)
        self.assertEqual(cedar_report[0].test_name, "BM_Name2")

    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.CheckPerfResultTestCase"
    )
    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    def test_check_pass_fail_handles_missing_project_name(
        self, mock_config, MockCheckPerfResultTestCase
    ):
        mock_config.EVERGREEN_PROJECT_NAME = None
        mock_config.EVERGREEN_REQUESTER = "github_pr"
        mock_config.EVERGREEN_REVISION = "abc123"

        self.cbr_hook.variant = "test-variant"
        self.cbr_hook.performance_thresholds = {
            "BM_Name1/arg1/arg with space": {
                "test-variant": [
                    {
                        "thread_level": 1,
                        "metrics": [
                            {
                                "name": "latency",
                                "bound_direction": "upper",
                                "threshold_limit": 500,
                            }
                        ],
                    }
                ]
            }
        }

        benchmark_reports = self.cbr_hook._parse_report(_BM_FULL_REPORT)
        cedar_formatted_results = self.cbr_hook._generate_cedar_report(benchmark_reports)

        with self.assertLogs("hook_logger", level="ERROR") as cm:
            self.cbr_hook._check_pass_fail(
                benchmark_reports,
                cedar_formatted_results,
                mock.MagicMock(),
                mock.MagicMock(),
            )

        self.assertTrue(
            any("Unable to determine the Evergreen project name" in msg for msg in cm.output),
            f"Expected error about missing project name in {cm.output}",
        )
        # A dynamic test case is still created but with an empty thresholds list, so it passes.
        MockCheckPerfResultTestCase.create_after_test.assert_called_once()
        mock_hook_test_case = MockCheckPerfResultTestCase.create_after_test.return_value
        mock_hook_test_case.run_dynamic_test.assert_called_once()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.CheckPerfResultTestCase"
    )
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.evergreen_conn.get_evergreen_api"
    )
    def test_check_pass_fail_resolves_version_id(
        self, mock_get_api, MockCheckPerfResultTestCase, mock_config
    ):
        # The version ID is resolved from the Evergreen API so that it is correct for any project,
        # including staging projects whose identifiers Evergreen sanitizes in non-obvious ways.
        mock_config.EVERGREEN_PROJECT_NAME = "mongodb-mongo-master-v8.3-staging"
        mock_config.EVERGREEN_REQUESTER = "github_pr"
        mock_config.EVERGREEN_REVISION = "abc123"

        self.cbr_hook.variant = "test-variant"
        self.cbr_hook.performance_thresholds = {
            "BM_Name1/arg1/arg with space": {
                "test-variant": [
                    {
                        "thread_level": 1,
                        "metrics": [
                            {
                                "name": "latency",
                                "bound_direction": "upper",
                                "threshold_limit": 500,
                            }
                        ],
                    }
                ]
            }
        }

        mock_task = mock.MagicMock()
        mock_task.version_id = "mongodb_mongo_master_v8_3_staging_abc123"
        mock_get_api.return_value.tasks_by_project_and_commit.return_value = [mock_task]

        mock_hook_test_case = mock.MagicMock()
        MockCheckPerfResultTestCase.create_after_test.return_value = mock_hook_test_case

        benchmark_reports = self.cbr_hook._parse_report(_BM_FULL_REPORT)
        cedar_formatted_results = self.cbr_hook._generate_cedar_report(benchmark_reports)

        with mock.patch.object(
            self.cbr_hook, "_retrieve_base_commit_value", return_value=50
        ) as mock_retrieve:
            self.cbr_hook._check_pass_fail(
                benchmark_reports,
                cedar_formatted_results,
                mock.MagicMock(),
                mock.MagicMock(),
            )

            mock_get_api.return_value.tasks_by_project_and_commit.assert_called_once_with(
                "mongodb-mongo-master-v8.3-staging", "abc123"
            )
            mock_retrieve.assert_called_once_with(
                test_name="BM_Name1/arg1/arg with space",
                task_name=cbr.SEP_BENCHMARKS_TASK_NAME,
                variant="test-variant",
                measurement="latency",
                args={"thread_level": 1},
                project="mongodb-mongo-master-v8.3-staging",
                version_id="mongodb_mongo_master_v8_3_staging_abc123",
            )

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.CheckPerfResultTestCase"
    )
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.evergreen_conn.get_evergreen_api"
    )
    def test_check_pass_fail_handles_version_resolution_failure(
        self, mock_get_api, MockCheckPerfResultTestCase, mock_config
    ):
        # If the Evergreen API lookup fails, version_id is None and the retrieve step falls back.
        mock_config.EVERGREEN_PROJECT_NAME = "mongodb-mongo-master"
        mock_config.EVERGREEN_REQUESTER = "github_pr"
        mock_config.EVERGREEN_REVISION = "abc123"

        self.cbr_hook.variant = "test-variant"
        self.cbr_hook.performance_thresholds = {
            "BM_Name1/arg1/arg with space": {
                "test-variant": [
                    {
                        "thread_level": 1,
                        "metrics": [
                            {
                                "name": "latency",
                                "bound_direction": "upper",
                                "threshold_limit": 500,
                            }
                        ],
                    }
                ]
            }
        }

        mock_get_api.side_effect = Exception("API error")

        mock_hook_test_case = mock.MagicMock()
        MockCheckPerfResultTestCase.create_after_test.return_value = mock_hook_test_case

        benchmark_reports = self.cbr_hook._parse_report(_BM_FULL_REPORT)
        cedar_formatted_results = self.cbr_hook._generate_cedar_report(benchmark_reports)

        with mock.patch.object(
            self.cbr_hook, "_retrieve_base_commit_value", return_value=50
        ) as mock_retrieve:
            # The resolution failure must be logged at ERROR so it reaches alerting.
            with self.assertLogs("hook_logger", level="ERROR") as log_ctx:
                self.cbr_hook._check_pass_fail(
                    benchmark_reports,
                    cedar_formatted_results,
                    mock.MagicMock(),
                    mock.MagicMock(),
                )

            mock_retrieve.assert_called_once_with(
                test_name="BM_Name1/arg1/arg with space",
                task_name=cbr.SEP_BENCHMARKS_TASK_NAME,
                variant="test-variant",
                measurement="latency",
                args={"thread_level": 1},
                project="mongodb-mongo-master",
                version_id=None,
            )
            self.assertTrue(
                any("Failed to resolve base commit" in line for line in log_ctx.output),
                f"expected an ERROR for the resolution failure, got: {log_ctx.output}",
            )

    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.evergreen_conn.get_evergreen_api"
    )
    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    def test_check_pass_fail_passes_when_no_thresholds_set(self, mock_config, mock_get_api):
        # The hook should pass when no thresholds are set for any benchmark, even if the project
        # name is unavailable. The project-name failure (and the base-version resolution) must be
        # deferred until at least one threshold check will actually run.
        mock_config.EVERGREEN_PROJECT_NAME = None

        self.cbr_hook.variant = "test-variant"
        self.cbr_hook.performance_thresholds = {}

        benchmark_reports = self.cbr_hook._parse_report(_BM_FULL_REPORT)
        cedar_formatted_results = self.cbr_hook._generate_cedar_report(benchmark_reports)

        # Should not raise even though the project name is None, because no checks run.
        self.cbr_hook._check_pass_fail(
            benchmark_reports,
            cedar_formatted_results,
            mock.MagicMock(),
            mock.MagicMock(),
        )

        self.assertFalse(self.cbr_hook.has_checked_results)
        # No threshold check ran, so the base version (an Evergreen API call) was never resolved.
        mock_get_api.assert_not_called()

    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.evergreen_conn.get_evergreen_api"
    )
    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    def test_check_pass_fail_passes_when_no_thresholds_for_variant(self, mock_config, mock_get_api):
        # Thresholds exist for the benchmark but not for the variant being run, so no check runs and
        # the hook should pass without resolving the project or base version.
        mock_config.EVERGREEN_PROJECT_NAME = None

        self.cbr_hook.variant = "test-variant"
        self.cbr_hook.performance_thresholds = {
            "BM_Name1/arg1/arg with space": {
                "some-other-variant": [
                    {
                        "thread_level": 1,
                        "metrics": [
                            {
                                "name": "latency",
                                "bound_direction": "upper",
                                "threshold_limit": 500,
                            }
                        ],
                    }
                ]
            }
        }

        benchmark_reports = self.cbr_hook._parse_report(_BM_FULL_REPORT)
        cedar_formatted_results = self.cbr_hook._generate_cedar_report(benchmark_reports)

        # Should not raise even though the project name is None, because no checks run.
        self.cbr_hook._check_pass_fail(
            benchmark_reports,
            cedar_formatted_results,
            mock.MagicMock(),
            mock.MagicMock(),
        )

        self.assertFalse(self.cbr_hook.has_checked_results)
        mock_get_api.assert_not_called()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.CheckPerfResultTestCase"
    )
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.evergreen_conn.get_evergreen_api"
    )
    def test_check_pass_fail_missing_baseline_logs_error(
        self, mock_get_api, MockCheckPerfResultTestCase, mock_config
    ):
        # The commit-queue use case: a missing base value must not fail the task, but must emit a
        # clear ERROR-level log that alerting can match on.
        mock_config.EVERGREEN_PROJECT_NAME = "mongodb-mongo-master"
        mock_config.EVERGREEN_REQUESTER = "github_pr"
        mock_config.EVERGREEN_REVISION = "abc123"

        self.cbr_hook.variant = "test-variant"
        self.cbr_hook.performance_thresholds = {
            "BM_Name1/arg1/arg with space": {
                "test-variant": [
                    {
                        "thread_level": 1,
                        "metrics": [
                            {
                                "name": "latency",
                                "bound_direction": "upper",
                                "threshold_limit": 500,
                            }
                        ],
                    }
                ]
            }
        }

        mock_task = mock.MagicMock()
        mock_task.version_id = "mongodb_mongo_master_abc123"
        mock_get_api.return_value.tasks_by_project_and_commit.return_value = [mock_task]
        MockCheckPerfResultTestCase.create_after_test.return_value = mock.MagicMock()

        benchmark_reports = self.cbr_hook._parse_report(_BM_FULL_REPORT)
        cedar_formatted_results = self.cbr_hook._generate_cedar_report(benchmark_reports)

        with mock.patch.object(self.cbr_hook, "_retrieve_base_commit_value", return_value=None):
            with self.assertLogs("hook_logger", level="ERROR") as log_ctx:
                self.cbr_hook._check_pass_fail(
                    benchmark_reports,
                    cedar_formatted_results,
                    mock.MagicMock(),
                    mock.MagicMock(),
                )

        self.assertTrue(
            any("no raw perf result found" in line for line in log_ctx.output),
            f"expected an alertable 'no raw perf result found' error, got: {log_ctx.output}",
        )

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.CheckPerfResultTestCase"
    )
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.evergreen_conn.get_evergreen_api"
    )
    def test_missing_baseline_does_not_fail_suite(
        self, mock_get_api, MockCheckPerfResultTestCase, mock_config
    ):
        # End-to-end: even with check_result=True (as in benchmarks_sep, which runs in the commit
        # queue), a missing base value must not block the task. The metric still counts as checked
        # so the after_suite reconciliation passes; alerting is expected to key off the ERROR log.
        mock_config.EVERGREEN_PROJECT_NAME = "mongodb-mongo-master"
        mock_config.EVERGREEN_REQUESTER = "github_pr"
        mock_config.EVERGREEN_REVISION = "abc123"

        self.cbr_hook.variant = "test-variant"
        self.cbr_hook.check_result = True
        self.cbr_hook.cedar_report_file = None
        self.cbr_hook.performance_thresholds = {
            "BM_Name1/arg1/arg with space": {
                "test-variant": [
                    {
                        "thread_level": 1,
                        "metrics": [
                            {
                                "name": "latency",
                                "bound_direction": "upper",
                                "threshold_limit": 500,
                            }
                        ],
                    }
                ]
            }
        }

        mock_task = mock.MagicMock()
        mock_task.version_id = "mongodb_mongo_master_abc123"
        mock_get_api.return_value.tasks_by_project_and_commit.return_value = [mock_task]
        MockCheckPerfResultTestCase.create_after_test.return_value = mock.MagicMock()

        benchmark_reports = self.cbr_hook._parse_report(_BM_FULL_REPORT)
        cedar_formatted_results = self.cbr_hook._generate_cedar_report(benchmark_reports)

        with mock.patch.object(self.cbr_hook, "_retrieve_base_commit_value", return_value=None):
            self.cbr_hook._check_pass_fail(
                benchmark_reports,
                cedar_formatted_results,
                mock.MagicMock(),
                mock.MagicMock(),
            )

        # A skipped-for-missing-baseline metric still counts as checked, so after_suite must not raise.
        self.assertTrue(self.cbr_hook.has_checked_results)
        self.cbr_hook.after_suite(mock.MagicMock())

    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.evergreen_conn.get_evergreen_api"
    )
    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    def test_resolve_base_version_skips_api_when_no_base_commit(self, mock_config, mock_get_api):
        # When there is no base commit to resolve (e.g. EVERGREEN_REVISION unset, as in a local run),
        # don't call the Evergreen API with a None revision. Return version_id None so the caller
        # skips the comparison, but avoid the pointless 404.
        mock_config.EVERGREEN_PROJECT_NAME = "mongodb-mongo-master"
        mock_config.EVERGREEN_REQUESTER = "patch"  # not a mainline requester
        mock_config.EVERGREEN_REVISION = None
        self.cbr_hook.variant = "test-variant"

        project, version_id = self.cbr_hook._resolve_base_version()

        self.assertEqual(project, "mongodb-mongo-master")
        self.assertIsNone(version_id)
        mock_get_api.assert_not_called()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.CheckPerfResultTestCase"
    )
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.evergreen_conn.get_evergreen_api"
    )
    def test_base_version_resolved_once_across_benchmark_files(
        self, mock_get_api, MockCheckPerfResultTestCase, mock_config
    ):
        # _check_pass_fail runs once per benchmark file (via after_test). The base commit is constant
        # for the whole run, so the Evergreen version is resolved once and cached across calls.
        mock_config.EVERGREEN_PROJECT_NAME = "mongodb-mongo-master"
        mock_config.EVERGREEN_REQUESTER = "github_pr"
        mock_config.EVERGREEN_REVISION = "abc123"

        self.cbr_hook.variant = "test-variant"
        self.cbr_hook.performance_thresholds = {
            "BM_Name1/arg1/arg with space": {
                "test-variant": [
                    {
                        "thread_level": 1,
                        "metrics": [
                            {
                                "name": "latency",
                                "bound_direction": "upper",
                                "threshold_limit": 500,
                            }
                        ],
                    }
                ]
            }
        }

        mock_task = mock.MagicMock()
        mock_task.version_id = "mongodb_mongo_master_abc123"
        mock_get_api.return_value.tasks_by_project_and_commit.return_value = [mock_task]
        MockCheckPerfResultTestCase.create_after_test.return_value = mock.MagicMock()

        benchmark_reports = self.cbr_hook._parse_report(_BM_FULL_REPORT)
        cedar_formatted_results = self.cbr_hook._generate_cedar_report(benchmark_reports)

        with mock.patch.object(self.cbr_hook, "_retrieve_base_commit_value", return_value=50):
            # Two benchmark files in the same run -> two _check_pass_fail calls.
            self.cbr_hook._check_pass_fail(
                benchmark_reports, cedar_formatted_results, mock.MagicMock(), mock.MagicMock()
            )
            self.cbr_hook._check_pass_fail(
                benchmark_reports, cedar_formatted_results, mock.MagicMock(), mock.MagicMock()
            )

        mock_get_api.return_value.tasks_by_project_and_commit.assert_called_once_with(
            "mongodb-mongo-master", "abc123"
        )

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.Github")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    def test_is_revert_build_true_for_revert_pr(
        self, mock_config, mock_get_expansion, mock_github_cls
    ):
        """_is_revert_build returns True when the PR title starts with 'Revert'."""
        mock_config.EVERGREEN_REQUESTER = "github_pr"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)
        mock_github_cls.return_value = _build_pr_title_github('Revert "SERVER-12345 Some change"')

        self.assertTrue(self.cbr_hook._is_revert_build())
        mock_github_cls.return_value.get_repo.assert_called_once_with(cbr.GITHUB_REPO_NAME)
        mock_github_cls.return_value.get_repo.return_value.get_pull.assert_called_once_with(12345)

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.Github")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    def test_is_revert_build_false_for_normal_pr(
        self, mock_config, mock_get_expansion, mock_github_cls
    ):
        """_is_revert_build returns False when the PR title does not start with 'Revert'."""
        mock_config.EVERGREEN_REQUESTER = "github_pr"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)
        mock_github_cls.return_value = _build_pr_title_github("SERVER-12345 Some change")

        self.assertFalse(self.cbr_hook._is_revert_build())

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.Github")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    def test_is_revert_build_false_for_revert_prefixed_words(
        self, mock_config, mock_get_expansion, mock_github_cls
    ):
        """_is_revert_build returns False for titles like 'Reverted' or 'Revertible'."""
        mock_config.EVERGREEN_REQUESTER = "github_pr"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)
        mock_github_cls.return_value = _build_pr_title_github("Revertible optimization")

        self.assertFalse(self.cbr_hook._is_revert_build())

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.Github")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    def test_is_revert_build_caches_pr_result(
        self, mock_config, mock_get_expansion, mock_github_cls
    ):
        """_is_revert_build caches the result so the GitHub API is only called once."""
        mock_config.EVERGREEN_REQUESTER = "github_pr"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)
        mock_github_cls.return_value = _build_pr_title_github("Revert something")

        self.cbr_hook._is_revert_build()
        self.cbr_hook._is_revert_build()
        mock_github_cls.return_value.get_repo.return_value.get_pull.assert_called_once()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.Github")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    def test_is_revert_build_pr_api_error(self, mock_config, mock_get_expansion, mock_github_cls):
        """_is_revert_build returns False and logs an error when the GitHub API fails."""
        mock_config.EVERGREEN_REQUESTER = "github_pr"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)
        mock_github_cls.return_value.get_repo.side_effect = GithubException(
            500, data={"message": "Server Error"}, headers={}
        )

        with self.assertLogs("hook_logger", level="ERROR") as cm:
            self.assertFalse(self.cbr_hook._is_revert_build())
        self.assertTrue(
            any("Failed to check PR title for revert" in msg for msg in cm.output),
            f"Expected error about GitHub API failure in {cm.output}",
        )

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.Github")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    def test_is_revert_build_missing_pr_number(
        self, mock_config, mock_get_expansion, mock_github_cls
    ):
        """_is_revert_build returns False and logs an error when github_pr_number is missing."""
        mock_config.EVERGREEN_REQUESTER = "github_pr"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_token_mongo": "fake_token",
        }.get(key, default)

        with self.assertLogs("hook_logger", level="ERROR") as cm:
            self.assertFalse(self.cbr_hook._is_revert_build())
        self.assertTrue(
            any("github_pr_number" in msg for msg in cm.output),
            f"Expected error about missing PR number in {cm.output}",
        )
        mock_github_cls.assert_not_called()

    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.subprocess.check_output"
    )
    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    def test_is_revert_build_true_for_revert_commit(self, mock_config, mock_check_output):
        """_is_revert_build returns True for mainline revert commits (git log fallback)."""
        mock_config.EVERGREEN_REQUESTER = "commit"
        mock_check_output.return_value = 'Revert "SERVER-12345 Some change"'
        self.assertTrue(self.cbr_hook._is_revert_build())
        mock_check_output.assert_called_once()

    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.subprocess.check_output"
    )
    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    def test_is_revert_build_false_for_normal_commit(self, mock_config, mock_check_output):
        """_is_revert_build returns False for normal mainline commits."""
        mock_config.EVERGREEN_REQUESTER = "commit"
        mock_check_output.return_value = "SERVER-12345 Some change"
        self.assertFalse(self.cbr_hook._is_revert_build())

    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.subprocess.check_output"
    )
    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    def test_is_revert_build_handles_git_error(self, mock_config, mock_check_output):
        """_is_revert_build returns False and logs an error when git log fails."""
        mock_config.EVERGREEN_REQUESTER = "commit"
        mock_check_output.side_effect = subprocess.CalledProcessError(1, "git")
        with self.assertLogs("hook_logger", level="ERROR") as cm:
            self.assertFalse(self.cbr_hook._is_revert_build())
        self.assertTrue(
            any("Failed to check if this is a revert commit" in msg for msg in cm.output),
            f"Expected error about git failure in {cm.output}",
        )

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.Github")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.CheckPerfResultTestCase"
    )
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.evergreen_conn.get_evergreen_api"
    )
    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    def test_check_pass_fail_skips_for_revert_pr(
        self,
        mock_config,
        mock_get_api,
        MockCheckPerfResultTestCase,
        mock_get_expansion,
        mock_github_cls,
    ):
        """Revert PRs skip threshold checks: _before_suite_impl detects the revert and returns
        early without loading thresholds, so _check_pass_fail finds no thresholds to check."""
        mock_config.EVERGREEN_PROJECT_NAME = "mongodb-mongo-master"
        mock_config.EVERGREEN_REQUESTER = "github_pr"
        mock_config.EVERGREEN_REVISION = "abc123"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)
        mock_github_cls.return_value = _build_pr_title_github('Revert "SERVER-12345 Some change"')

        self.cbr_hook.variant = "test-variant"

        # _before_suite_impl detects the revert and returns before loading thresholds.
        self.cbr_hook._before_suite_impl(mock.MagicMock())
        self.assertEqual(self.cbr_hook.performance_thresholds, {})

        benchmark_reports = self.cbr_hook._parse_report(_BM_FULL_REPORT)
        cedar_formatted_results = self.cbr_hook._generate_cedar_report(benchmark_reports)

        self.cbr_hook._check_pass_fail(
            benchmark_reports,
            cedar_formatted_results,
            mock.MagicMock(),
            mock.MagicMock(),
        )

        # No dynamic test case should be created, no Evergreen API call.
        MockCheckPerfResultTestCase.create_after_test.assert_not_called()
        mock_get_api.assert_not_called()
        self.assertFalse(self.cbr_hook.has_checked_results)

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.Github")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    def test_after_suite_no_error_for_revert_pr(
        self, mock_config, mock_get_expansion, mock_github_cls
    ):
        """_after_suite_impl does not log a 'no results checked' error for revert PRs."""
        mock_config.EVERGREEN_PROJECT_NAME = "mongodb-mongo-master"
        mock_config.EVERGREEN_REQUESTER = "github_pr"
        mock_config.EVERGREEN_REVISION = "abc123"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)
        mock_github_cls.return_value = _build_pr_title_github('Revert "SERVER-12345 Some change"')

        self.cbr_hook.variant = "test-variant"
        self.cbr_hook.check_result = True
        self.cbr_hook.cedar_report_file = None

        # _before_suite_impl detects the revert and returns early without loading thresholds.
        self.cbr_hook._before_suite_impl(mock.MagicMock())
        self.assertEqual(self.cbr_hook.performance_thresholds, {})

        benchmark_reports = self.cbr_hook._parse_report(_BM_FULL_REPORT)
        cedar_formatted_results = self.cbr_hook._generate_cedar_report(benchmark_reports)

        # _check_pass_fail finds no thresholds to check, so has_checked_results stays False.
        self.cbr_hook._check_pass_fail(
            benchmark_reports,
            cedar_formatted_results,
            mock.MagicMock(),
            mock.MagicMock(),
        )
        self.assertFalse(self.cbr_hook.has_checked_results)

        # after_suite should NOT log an ERROR even though check_result=True and
        # has_checked_results=False, because this is a revert build.
        with self.assertNoLogs("hook_logger", level="ERROR"):
            self.cbr_hook.after_suite(mock.MagicMock())


class TestBenchmarkThreadsReport(GenerateAndCheckPerfResultsFixture):
    def test_thread_from_name(self):
        name_obj = self.bm_threads_report.parse_bm_name({"name": "BM_Name/arg name:100/threads:10"})
        self.assertEqual(name_obj.thread_count, "10")
        self.assertEqual(name_obj.statistic_type, None)
        self.assertEqual(name_obj.base_name, "BM_Name/arg name:100")

        name_obj = self.bm_threads_report.parse_bm_name(
            {"name": "BM_Name/arg name:100/threads:10_mean", "aggregate_name": "mean"}
        )
        self.assertEqual(name_obj.thread_count, "10")
        self.assertEqual(name_obj.statistic_type, "mean")
        self.assertEqual(name_obj.base_name, "BM_Name/arg name:100")

        name_obj = self.bm_threads_report.parse_bm_name({"name": "BM_Name/threads:abcd"})
        self.assertEqual(name_obj.thread_count, "abcd")
        self.assertEqual(name_obj.statistic_type, None)
        self.assertEqual(name_obj.base_name, "BM_Name")

        name_obj = self.bm_threads_report.parse_bm_name({"name": "BM_Name"})
        self.assertEqual(name_obj.thread_count, "1")
        self.assertEqual(name_obj.statistic_type, None)
        self.assertEqual(name_obj.base_name, "BM_Name")

        name_obj = self.bm_threads_report.parse_bm_name(
            {"name": "BM_Name/1/eeee_mean", "aggregate_name": "mean"}
        )
        self.assertEqual(name_obj.thread_count, "1")
        self.assertEqual(name_obj.statistic_type, "mean")
        self.assertEqual(name_obj.base_name, "BM_Name/1/eeee")

        name_obj = self.bm_threads_report.parse_bm_name({"name": "BM_Name/arg name:100"})
        self.assertEqual(name_obj.thread_count, "1")
        self.assertEqual(name_obj.statistic_type, None)
        self.assertEqual(name_obj.base_name, "BM_Name/arg name:100")

        name_obj = self.bm_threads_report.parse_bm_name({"name": "BM_baseline_match_simple/0"})
        self.assertEqual(name_obj.thread_count, "1")
        self.assertEqual(name_obj.statistic_type, None)
        self.assertEqual(name_obj.base_name, "BM_baseline_match_simple/0")

        name_obj = self.bm_threads_report.parse_bm_name(
            {"name": "BM_baseline_match_simple/0_mean", "aggregate_name": "mean"}
        )
        self.assertEqual(name_obj.thread_count, "1")
        self.assertEqual(name_obj.statistic_type, "mean")
        self.assertEqual(name_obj.base_name, "BM_baseline_match_simple/0")

    def test_iterations_from_name(self):
        name_obj = self.bm_threads_report.parse_bm_name(
            {"name": "BM_Name/iterations:10000/threads:10"}
        )
        self.assertEqual(name_obj.thread_count, "10")
        self.assertEqual(name_obj.statistic_type, None)
        self.assertEqual(name_obj.base_name, "BM_Name")

        name_obj = self.bm_threads_report.parse_bm_name(
            {
                "name": "BM_Name/arg name:100/iterations:10000/threads:10_mean",
                "aggregate_name": "mean",
            }
        )
        self.assertEqual(name_obj.thread_count, "10")
        self.assertEqual(name_obj.statistic_type, "mean")
        self.assertEqual(name_obj.base_name, "BM_Name/arg name:100")

        name_obj = self.bm_threads_report.parse_bm_name({"name": "BM_Name/iterations:10000"})
        self.assertEqual(name_obj.thread_count, "1")
        self.assertEqual(name_obj.statistic_type, None)
        self.assertEqual(name_obj.base_name, "BM_Name")

    def test_generate_multithread_cedar_metrics(self):
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_MULTITHREAD_REPORT), _BM_MULTITHREAD_REPORT
        )
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_MULTITHREAD_MEDIAN_REPORT),
            _BM_MULTITHREAD_MEDIAN_REPORT,
        )
        self.assertEqual(len(self.bm_threads_report.thread_benchmark_map.keys()), 1)

        cedar_metrics = self.bm_threads_report.generate_cedar_metrics()

        self.assertIn(10, cedar_metrics)
        self.assertEqual(len(cedar_metrics[10]), 2)
        collected_types = (m.type for m in cedar_metrics[10])
        self.assertIn("LATENCY", collected_types)
        self.assertIn("MEDIAN", collected_types)

    def test_generate_single_thread_cedar_metrics(self):
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_1), _BM_REPORT_1
        )
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_2), _BM_REPORT_2
        )
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_MEAN_REPORT), _BM_MEAN_REPORT
        )
        self.assertEqual(len(self.bm_threads_report.thread_benchmark_map.keys()), 1)

        cedar_metrics = self.bm_threads_report.generate_cedar_metrics()

        self.assertIn(1, cedar_metrics)
        self.assertEqual(len(cedar_metrics[1]), 3)
        collected_types = (m.type for m in cedar_metrics[1])
        self.assertIn("LATENCY", collected_types)
        self.assertIn("MEAN", collected_types)

    def test_generate_cedar_report_with_instructions(self):
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_WITH_INSTRUCTIONS_1),
            _BM_REPORT_WITH_INSTRUCTIONS_1,
        )
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_WITH_INSTRUCTIONS_2),
            _BM_REPORT_WITH_INSTRUCTIONS_2,
        )
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_WITH_INSTRUCTIONS_MEAN),
            _BM_REPORT_WITH_INSTRUCTIONS_MEAN,
        )
        self.assertEqual(len(self.bm_threads_report.thread_benchmark_map.keys()), 1)

        cedar_metrics = self.bm_threads_report.generate_cedar_metrics()

        self.assertIn(1, cedar_metrics)
        self.assertEqual(len(cedar_metrics[1]), 2)
        collected_types = (m.type for m in cedar_metrics[2])
        self.assertIn("LATENCY", collected_types)
        self.assertIn("MEAN", collected_types)

    def test_generate_cedar_report_with_cycles(self):
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_WITH_CYCLES_1), _BM_REPORT_WITH_CYCLES_1
        )
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_WITH_CYCLES_2), _BM_REPORT_WITH_CYCLES_2
        )
        self.bm_threads_report.add_report(
            self.bm_threads_report.parse_bm_name(_BM_REPORT_WITH_CYCLES_MEAN),
            _BM_REPORT_WITH_CYCLES_MEAN,
        )
        self.assertEqual(len(self.bm_threads_report.thread_benchmark_map.keys()), 1)

        cedar_metrics = self.bm_threads_report.generate_cedar_metrics()

        self.assertIn(1, cedar_metrics)
        self.assertEqual(len(cedar_metrics[1]), 2)
        collected_types = (m.type for m in cedar_metrics[2])
        self.assertIn("LATENCY", collected_types)
        self.assertIn("MEAN", collected_types)


def _make_comment(body, login):
    """Build a mock PR comment with the given body and author login."""
    comment = mock.MagicMock()
    comment.body = body
    comment.user.login = login
    return comment


def _build_pr_title_github(title, pr_number=12345):
    """Build a mock GitHub client whose PR has the given title (for revert-title checks)."""
    mock_github = mock.MagicMock()
    mock_pr = mock.MagicMock()
    mock_pr.number = pr_number
    mock_pr.title = title
    mock_repo = mock.MagicMock()
    mock_repo.get_pull.return_value = mock_pr
    mock_github.get_repo.return_value = mock_repo
    return mock_github


def _build_override_github(
    *,
    issue_comments=(),
    review_comments=(),
    commit_comments=(),
    active_members=(),
    pending_members=(),
    membership_error=None,
):
    """Build a mock GitHub client for exercising the override-check path.

    active_members:   logins resolved as active members of the approver team.
    pending_members:  logins resolved as having a non-active ('pending') membership.
    membership_error: if set, every get_team_membership call raises this exception.
    Any other login raises GithubException(404), i.e. "not a member".
    """
    mock_github = mock.MagicMock()
    mock_pr = mock.MagicMock()
    mock_repo = mock.MagicMock()
    mock_pr.number = 12345
    mock_pr.get_issue_comments.return_value = list(issue_comments)
    mock_pr.get_review_comments.return_value = list(review_comments)
    mock_pr.get_comments.return_value = list(commit_comments)
    mock_repo.get_pull.return_value = mock_pr
    mock_github.get_repo.return_value = mock_repo

    active = set(active_members)
    pending = set(pending_members)

    def _get_team_membership(login):
        if membership_error is not None:
            raise membership_error
        if login in active:
            membership = mock.MagicMock()
            membership.state = "active"
            return membership
        if login in pending:
            membership = mock.MagicMock()
            membership.state = "pending"
            return membership
        raise GithubException(404, data={"message": "Not Found"}, headers={})

    mock_team = mock.MagicMock()
    mock_team.get_team_membership.side_effect = _get_team_membership
    mock_github.get_organization.return_value.get_team_by_slug.return_value = mock_team
    return mock_github


def _build_failing_test_case():
    """Build a CheckPerfResultTestCase whose single metric exceeds its threshold."""
    thresholds_to_check: list[cbr.IndividualMetricThreshold] = [
        cbr.IndividualMetricThreshold(
            metric_name="latency",
            thread_level=1,
            test_name="fake-test",
            value=10,
            bound_direction="upper",
            threshold_limit=20,
        )
    ]
    reported_metrics: dict[cbr.ReportedMetric, CedarMetric] = {
        cbr.ReportedMetric(
            test_name="fake-test", thread_level=1, metric_name="latency"
        ): CedarMetric(name="latency", type="LATENCY", value=100)
    }
    return cbr.CheckPerfResultTestCase(
        logging.getLogger("hook_logger"),
        "my-test",
        None,
        None,
        None,
        thresholds_to_check,
        reported_metrics,
    )


class TestRetrieveBaseCommitValue(GenerateAndCheckPerfResultsFixture):
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.requests.get"
    )
    def test_retrieve_base_commit_value_success(self, mock_get):
        """Test successful retrieval of an exact parent value from raw perf results."""
        mock_response = mock.MagicMock()
        mock_response.json.return_value = [
            {
                "info": {
                    "project": "mongodb-mongo-master",
                    "variant": "my-variant",
                    "task_name": "benchmarks_sep",
                    "test_name": "BM_Test",
                    "args": {"thread_level": 1},
                },
                "rollups": {
                    "stats": [
                        {"name": "latency", "val": 42},
                    ]
                },
            }
        ]
        mock_response.raise_for_status.return_value = None
        mock_get.return_value = mock_response

        value = self.cbr_hook._retrieve_base_commit_value(
            test_name="BM_Test",
            task_name="benchmarks_sep",
            variant="my-variant",
            measurement="latency",
            args={"thread_level": 1},
            project="mongodb-mongo-master",
            version_id="some_version_id",
        )

        self.assertEqual(value, 42)
        mock_get.assert_called_once()
        call_args = mock_get.call_args
        self.assertIn("some_version_id", call_args[0][0])
        self.assertEqual(call_args[1]["params"]["test_name"], "BM_Test")
        self.assertEqual(call_args[1]["params"]["filter_stats_name"], "latency")

    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.requests.get"
    )
    def test_retrieve_base_commit_value_project_id_mismatch(self, mock_get):
        """The project filter must not compare info.project to the hook's project parameter.

        SPS stores info.project as the Evergreen project_id (an ObjectId for newer projects, e.g.
        'mongodb-mongo-v8.3-staging' maps to '69b8579ce7a4080007a30200'), while the hook receives
        the human-readable project identifier. The /raw_perf_results/versions/{version_id}
        endpoint is already scoped to a single project, so the remaining filters on variant,
        task_name, test_name, and args are sufficient. A mismatch on info.project must not cause
        _retrieve_base_commit_value to return None and silently skip the threshold check.
        """
        mock_response = mock.MagicMock()
        mock_response.json.return_value = [
            {
                "info": {
                    "project": "69b8579ce7a4080007a30200",
                    "variant": "my-variant",
                    "task_name": "benchmarks_sep",
                    "test_name": "BM_Test",
                    "args": {"thread_level": 1},
                },
                "rollups": {
                    "stats": [
                        {"name": "latency", "val": 42},
                    ]
                },
            }
        ]
        mock_response.raise_for_status.return_value = None
        mock_get.return_value = mock_response

        value = self.cbr_hook._retrieve_base_commit_value(
            test_name="BM_Test",
            task_name="benchmarks_sep",
            variant="my-variant",
            measurement="latency",
            args={"thread_level": 1},
            project="mongodb-mongo-v8.3-staging",
            version_id="some_version_id",
        )

        self.assertEqual(value, 42)

    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.requests.get"
    )
    def test_retrieve_base_commit_value_uses_latest_execution(self, mock_get):
        """Test that the value from the latest execution is used when there are retries."""
        info = {
            "project": "mongodb-mongo-master",
            "variant": "my-variant",
            "task_name": "benchmarks_sep",
            "test_name": "BM_Test",
            "args": {"thread_level": 1},
        }
        mock_response = mock.MagicMock()
        # Deliberately out of execution order to prove selection is by execution, not list position.
        mock_response.json.return_value = [
            {
                "info": {**info, "execution": 0},
                "rollups": {"stats": [{"name": "latency", "val": 10}]},
            },
            {
                "info": {**info, "execution": 2},
                "rollups": {"stats": [{"name": "latency", "val": 30}]},
            },
            {
                "info": {**info, "execution": 1},
                "rollups": {"stats": [{"name": "latency", "val": 20}]},
            },
        ]
        mock_response.raise_for_status.return_value = None
        mock_get.return_value = mock_response

        value = self.cbr_hook._retrieve_base_commit_value(
            test_name="BM_Test",
            task_name="benchmarks_sep",
            variant="my-variant",
            measurement="latency",
            args={"thread_level": 1},
            project="mongodb-mongo-master",
            version_id="some_version_id",
        )

        self.assertEqual(value, 30)

    def test_retrieve_base_commit_value_no_version_id(self):
        """Test that None is returned when no version_id is provided."""
        with mock.patch(
            "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.requests.get"
        ) as mock_get:
            value = self.cbr_hook._retrieve_base_commit_value(
                test_name="BM_Test",
                task_name="benchmarks_sep",
                variant="my-variant",
                measurement="latency",
                args={"thread_level": 1},
                project="mongodb-mongo-master",
                version_id=None,
            )

            self.assertIsNone(value)
            mock_get.assert_not_called()

    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.requests.get"
    )
    def test_retrieve_base_commit_value_no_match(self, mock_get):
        """Test that None is returned when no matching result is found."""
        mock_response = mock.MagicMock()
        mock_response.json.return_value = [
            {
                "info": {
                    "project": "other-project",
                    "variant": "other-variant",
                    "task_name": "other_task",
                    "test_name": "Other_Test",
                    "args": {"thread_level": 2},
                },
                "rollups": {
                    "stats": [
                        {"name": "throughput", "val": 100},
                    ]
                },
            }
        ]
        mock_response.raise_for_status.return_value = None
        mock_get.return_value = mock_response

        value = self.cbr_hook._retrieve_base_commit_value(
            test_name="BM_Test",
            task_name="benchmarks_sep",
            variant="my-variant",
            measurement="latency",
            args={"thread_level": 1},
            project="mongodb-mongo-master",
            version_id="some_version_id",
        )

        self.assertIsNone(value)

    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.requests.get"
    )
    def test_retrieve_base_commit_value_null_rollups(self, mock_get):
        """Test that null rollups in API response does not crash."""
        mock_response = mock.MagicMock()
        mock_response.json.return_value = [
            {
                "info": {
                    "project": "mongodb-mongo-master",
                    "variant": "my-variant",
                    "task_name": "benchmarks_sep",
                    "test_name": "BM_Test",
                    "args": {"thread_level": 1},
                },
                "rollups": None,
            }
        ]
        mock_response.raise_for_status.return_value = None
        mock_get.return_value = mock_response

        value = self.cbr_hook._retrieve_base_commit_value(
            test_name="BM_Test",
            task_name="benchmarks_sep",
            variant="my-variant",
            measurement="latency",
            args={"thread_level": 1},
            project="mongodb-mongo-master",
            version_id="some_version_id",
        )

        self.assertIsNone(value)

    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.requests.get"
    )
    def test_retrieve_base_commit_value_null_info(self, mock_get):
        """Test that null info in API response does not crash."""
        mock_response = mock.MagicMock()
        mock_response.json.return_value = [
            {
                "info": None,
                "rollups": {"stats": [{"name": "latency", "val": 42}]},
            }
        ]
        mock_response.raise_for_status.return_value = None
        mock_get.return_value = mock_response

        value = self.cbr_hook._retrieve_base_commit_value(
            test_name="BM_Test",
            task_name="benchmarks_sep",
            variant="my-variant",
            measurement="latency",
            args={"thread_level": 1},
            project="mongodb-mongo-master",
            version_id="some_version_id",
        )

        self.assertIsNone(value)

    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.requests.get"
    )
    def test_retrieve_base_commit_value_request_failure(self, mock_get):
        """A request failure is non-blocking: it logs an ERROR and returns None rather than raising."""
        mock_get.side_effect = requests.RequestException("Connection error")

        with self.assertLogs("hook_logger", level="ERROR") as log_ctx:
            value = self.cbr_hook._retrieve_base_commit_value(
                test_name="BM_Test",
                task_name="benchmarks_sep",
                variant="my-variant",
                measurement="latency",
                args={"thread_level": 1},
                project="mongodb-mongo-master",
                version_id="some_version_id",
            )

        self.assertIsNone(value)
        self.assertTrue(
            any("Connection error" in line for line in log_ctx.output),
            f"expected an alertable error mentioning the failure, got: {log_ctx.output}",
        )

    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.requests.get"
    )
    def test_retrieve_base_commit_value_non_list_response(self, mock_get):
        """Test that a non-list (e.g. error-envelope) JSON body does not crash.

        raise_for_status only catches HTTP error codes; the endpoint can still return HTTP 200 with
        a dict body (e.g. {"message": ...}) when a version has no data. That must be treated as
        "no matching result" and return None, not raise AttributeError while iterating dict keys.
        """
        mock_response = mock.MagicMock()
        mock_response.json.return_value = {"message": "no results for this version"}
        mock_response.raise_for_status.return_value = None
        mock_get.return_value = mock_response

        value = self.cbr_hook._retrieve_base_commit_value(
            test_name="BM_Test",
            task_name="benchmarks_sep",
            variant="my-variant",
            measurement="latency",
            args={"thread_level": 1},
            project="mongodb-mongo-master",
            version_id="some_version_id",
        )

        self.assertIsNone(value)

    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.requests.get"
    )
    def test_retrieve_base_commit_value_null_execution(self, mock_get):
        """Test that a result with execution explicitly null does not crash latest-execution selection.

        info.get("execution", -1) only substitutes the default when the key is absent, so a
        present-but-null execution leaves None in the max() key and would compare None against an
        int. A null execution must be treated as the oldest, with the real numbered execution
        winning.
        """
        info = {
            "project": "mongodb-mongo-master",
            "variant": "my-variant",
            "task_name": "benchmarks_sep",
            "test_name": "BM_Test",
            "args": {"thread_level": 1},
        }
        mock_response = mock.MagicMock()
        mock_response.json.return_value = [
            {
                "info": {**info, "execution": None},
                "rollups": {"stats": [{"name": "latency", "val": 10}]},
            },
            {
                "info": {**info, "execution": 1},
                "rollups": {"stats": [{"name": "latency", "val": 20}]},
            },
        ]
        mock_response.raise_for_status.return_value = None
        mock_get.return_value = mock_response

        value = self.cbr_hook._retrieve_base_commit_value(
            test_name="BM_Test",
            task_name="benchmarks_sep",
            variant="my-variant",
            measurement="latency",
            args={"thread_level": 1},
            project="mongodb-mongo-master",
            version_id="some_version_id",
        )

        self.assertEqual(value, 20)


class TestCheckPerfResultTestCase(unittest.TestCase):
    def test_all_metrics_pass(self):
        thresholds_to_check: list[cbr.IndividualMetricThreshold] = [
            cbr.IndividualMetricThreshold(
                metric_name="latency",
                thread_level=1,
                test_name="fake-test",
                value=10,
                bound_direction="upper",
                threshold_limit=20,
            )
        ]
        reported_metrics: dict[cbr.ReportedMetric, CedarMetric] = {
            cbr.ReportedMetric(
                test_name="fake-test", thread_level=1, metric_name="latency"
            ): CedarMetric(name="latency", type="LATENCY", value=1)
        }
        test_case = cbr.CheckPerfResultTestCase(
            logging.getLogger("hook_logger"),
            "my-test",
            None,
            None,
            None,
            thresholds_to_check,
            reported_metrics,
        )

        # We want to make sure this can run without any exceptions.
        test_case.run_test()

    def test_a_metric_fails(self):
        thresholds_to_check: list[cbr.IndividualMetricThreshold] = [
            cbr.IndividualMetricThreshold(
                metric_name="latency",
                thread_level=1,
                test_name="fake-test",
                value=10,
                bound_direction="upper",
                threshold_limit=20,
            )
        ]
        reported_metrics: dict[cbr.ReportedMetric, CedarMetric] = {
            cbr.ReportedMetric(
                test_name="fake-test", thread_level=1, metric_name="latency"
            ): CedarMetric(name="latency", type="LATENCY", value=100)
        }
        test_case = cbr.CheckPerfResultTestCase(
            logging.getLogger("hook_logger"),
            "my-test",
            None,
            None,
            None,
            thresholds_to_check,
            reported_metrics,
        )

        with self.assertRaisesRegex(ServerFailure, "threshold check"):
            test_case.run_test()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    def test_override_comment_allows_failure_issue_comment(self, mock_get_expansion, mock_config):
        """Test that an override comment from a team member allows a metric failure (issue comment)."""
        mock_config.EVERGREEN_REQUESTER = "github_merge_queue"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)

        mock_github = _build_override_github(
            issue_comments=[
                _make_comment("This is a perf threshold check override comment", "team-member")
            ],
            active_members=["team-member"],
        )
        test_case = _build_failing_test_case()
        test_case.github = mock_github

        # Should not raise an exception due to override comment
        test_case.run_test()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    def test_override_comment_allows_failure_review_comment(self, mock_get_expansion, mock_config):
        """Test that an override comment from a team member allows a metric failure (review comment)."""
        mock_config.EVERGREEN_REQUESTER = "github_merge_queue"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)

        mock_github = _build_override_github(
            # Mixed case verifies the override phrase match is case-insensitive.
            review_comments=[_make_comment("PERF THRESHOLD CHECK OVERRIDE", "team-member")],
            active_members=["team-member"],
        )
        test_case = _build_failing_test_case()
        test_case.github = mock_github

        # Should not raise an exception due to override comment
        test_case.run_test()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    def test_override_comment_allows_failure_commit_comment(self, mock_get_expansion, mock_config):
        """Test that an override comment from a team member allows a metric failure (commit comment)."""
        mock_config.EVERGREEN_REQUESTER = "github_merge_queue"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)

        mock_github = _build_override_github(
            commit_comments=[
                _make_comment("perf threshold check override - approved", "team-member")
            ],
            active_members=["team-member"],
        )
        test_case = _build_failing_test_case()
        test_case.github = mock_github

        # Should not raise an exception due to override comment
        test_case.run_test()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    def test_override_comment_from_unauthorized_user_fails(self, mock_get_expansion, mock_config):
        """Test that an override comment from a non-team-member does not prevent failure."""
        mock_config.EVERGREEN_REQUESTER = "github_merge_queue"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)

        mock_github = _build_override_github(
            # Not a member of any approver team, so the membership lookup 404s.
            issue_comments=[_make_comment("perf threshold check override", "unauthorized-user")],
            active_members=[],
        )
        test_case = _build_failing_test_case()
        test_case.github = mock_github

        # Should raise an exception since user is not authorized
        with self.assertRaisesRegex(ServerFailure, "threshold check"):
            test_case.run_test()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    def test_no_override_comment_fails(self, mock_get_expansion, mock_config):
        """Test that a failure without override comment still fails, even from a team member."""
        mock_config.EVERGREEN_REQUESTER = "github_merge_queue"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)

        mock_github = _build_override_github(
            # A team member, but the comment lacks the override phrase.
            issue_comments=[
                _make_comment("This is a regular comment without override", "team-member")
            ],
            active_members=["team-member"],
        )
        test_case = _build_failing_test_case()
        test_case.github = mock_github

        # Should raise an exception since there's no override comment
        with self.assertRaisesRegex(ServerFailure, "threshold check"):
            test_case.run_test()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    def test_override_queries_configured_org_and_team(self, mock_get_expansion, mock_config):
        """Test that membership is checked against the configured org and team slug."""
        mock_config.EVERGREEN_REQUESTER = "github_merge_queue"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)

        mock_github = _build_override_github(
            issue_comments=[_make_comment("perf threshold check override", "team-member")],
            active_members=["team-member"],
        )
        test_case = _build_failing_test_case()
        test_case.github = mock_github

        test_case.run_test()

        mock_github.get_organization.assert_called_once_with(cbr.OVERRIDE_APPROVER_ORG)
        mock_github.get_organization.return_value.get_team_by_slug.assert_called_once_with(
            "performance"
        )

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    def test_override_member_of_any_configured_team_allows_failure(
        self, mock_get_expansion, mock_config
    ):
        """Test that membership in any one of several configured teams grants override."""
        mock_config.EVERGREEN_REQUESTER = "github_merge_queue"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)

        # The user is only a member of "second-team"; lookups against other teams 404.
        def get_team_by_slug(slug):
            team = mock.MagicMock()

            def membership(login):
                if slug == "second-team" and login == "team-member":
                    active = mock.MagicMock()
                    active.state = "active"
                    return active
                raise GithubException(404, data={"message": "Not Found"}, headers={})

            team.get_team_membership.side_effect = membership
            return team

        mock_github = mock.MagicMock()
        mock_pr = mock.MagicMock()
        mock_repo = mock.MagicMock()
        mock_pr.number = 12345
        mock_pr.get_issue_comments.return_value = [
            _make_comment("perf threshold check override", "team-member")
        ]
        mock_pr.get_review_comments.return_value = []
        mock_pr.get_comments.return_value = []
        mock_repo.get_pull.return_value = mock_pr
        mock_github.get_repo.return_value = mock_repo
        mock_github.get_organization.return_value.get_team_by_slug.side_effect = get_team_by_slug

        test_case = _build_failing_test_case()
        test_case.github = mock_github

        with mock.patch.object(
            cbr, "OVERRIDE_APPROVER_TEAMS", frozenset(["first-team", "second-team"])
        ):
            # Should not raise: the user is an active member of one of the teams.
            test_case.run_test()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    def test_override_pending_membership_denied(self, mock_get_expansion, mock_config):
        """Test that a 'pending' (non-active) team membership does not grant override."""
        mock_config.EVERGREEN_REQUESTER = "github_merge_queue"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)

        mock_github = _build_override_github(
            issue_comments=[_make_comment("perf threshold check override", "invitee")],
            pending_members=["invitee"],
        )
        test_case = _build_failing_test_case()
        test_case.github = mock_github

        with self.assertRaisesRegex(ServerFailure, "threshold check"):
            test_case.run_test()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    def test_override_membership_api_error_is_failsafe(self, mock_get_expansion, mock_config):
        """Test that a non-404 membership API error is treated as not authorized (fail-safe)."""
        mock_config.EVERGREEN_REQUESTER = "github_merge_queue"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_pr_number": "12345",
            "github_token_mongo": "fake_token",
        }.get(key, default)

        mock_github = _build_override_github(
            issue_comments=[_make_comment("perf threshold check override", "team-member")],
            membership_error=GithubException(403, data={"message": "Forbidden"}, headers={}),
        )
        test_case = _build_failing_test_case()
        test_case.github = mock_github

        # The threshold failure must stand, surfaced as ServerFailure (not the raw API error).
        with self.assertRaisesRegex(ServerFailure, "threshold check"):
            test_case.run_test()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    def test_override_not_checked_when_not_merge_queue(self, mock_get_expansion, mock_config):
        """Test that override checking is skipped when not in merge queue."""
        mock_config.EVERGREEN_REQUESTER = "patch_request"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_token_mongo": "fake_token",
        }.get(key, default)

        thresholds_to_check: list[cbr.IndividualMetricThreshold] = [
            cbr.IndividualMetricThreshold(
                metric_name="latency",
                thread_level=1,
                test_name="fake-test",
                value=10,
                bound_direction="upper",
                threshold_limit=20,
            )
        ]
        reported_metrics: dict[cbr.ReportedMetric, CedarMetric] = {
            cbr.ReportedMetric(
                test_name="fake-test", thread_level=1, metric_name="latency"
            ): CedarMetric(name="latency", type="LATENCY", value=100)
        }
        test_case = cbr.CheckPerfResultTestCase(
            logging.getLogger("hook_logger"),
            "my-test",
            None,
            None,
            None,
            thresholds_to_check,
            reported_metrics,
        )

        # Should raise an exception since we're not in merge queue
        with self.assertRaisesRegex(ServerFailure, "threshold check"):
            test_case.run_test()

    @mock.patch("buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results._config")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.generate_and_check_perf_results.get_expansion"
    )
    def test_missing_pr_number_logs_error(self, mock_get_expansion, mock_config):
        """Test that missing PR number logs an error but the threshold failure is still raised."""
        mock_config.EVERGREEN_REQUESTER = "github_merge_queue"
        mock_get_expansion.side_effect = lambda key, default=None: {
            "github_token_mongo": "fake_token",
        }.get(key, default)

        thresholds_to_check: list[cbr.IndividualMetricThreshold] = [
            cbr.IndividualMetricThreshold(
                metric_name="latency",
                thread_level=1,
                test_name="fake-test",
                value=10,
                bound_direction="upper",
                threshold_limit=20,
            )
        ]
        reported_metrics: dict[cbr.ReportedMetric, CedarMetric] = {
            cbr.ReportedMetric(
                test_name="fake-test", thread_level=1, metric_name="latency"
            ): CedarMetric(name="latency", type="LATENCY", value=100)
        }
        test_case = cbr.CheckPerfResultTestCase(
            logging.getLogger("hook_logger"),
            "my-test",
            None,
            None,
            None,
            thresholds_to_check,
            reported_metrics,
        )

        with self.assertLogs("hook_logger", level="ERROR") as cm:
            with self.assertRaisesRegex(ServerFailure, "threshold check"):
                test_case.run_test()

        self.assertTrue(
            any("github_pr_number" in msg for msg in cm.output),
            f"Expected error about missing PR number in {cm.output}",
        )

    def test_metric_doesnt_exist(self):
        thresholds_to_check: list[cbr.IndividualMetricThreshold] = [
            cbr.IndividualMetricThreshold(
                metric_name="latency",
                thread_level=1,
                test_name="fake-test",
                value=10,
                bound_direction="upper",
                threshold_limit=20,
            )
        ]
        reported_metrics: dict[cbr.ReportedMetric, CedarMetric] = {
            cbr.ReportedMetric(
                test_name="fake-test", thread_level=1, metric_name="instructions"
            ): CedarMetric(name="instructions", type="LATENCY", value=1)
        }
        test_case = cbr.CheckPerfResultTestCase(
            logging.getLogger("hook_logger"),
            "my-test",
            None,
            None,
            None,
            thresholds_to_check,
            reported_metrics,
        )

        with self.assertRaisesRegex(ServerFailure, "threshold check"):
            test_case.run_test()

    def test_thread_level_doesnt_exist(self):
        thresholds_to_check: list[cbr.IndividualMetricThreshold] = [
            cbr.IndividualMetricThreshold(
                metric_name="latency",
                thread_level=1,
                test_name="fake-test",
                value=10,
                bound_direction="upper",
                threshold_limit=20,
            )
        ]
        reported_metrics: dict[cbr.ReportedMetric, CedarMetric] = {
            cbr.ReportedMetric(
                test_name="fake-test", thread_level=12, metric_name="latency"
            ): CedarMetric(name="latency", type="LATENCY", value=1)
        }
        test_case = cbr.CheckPerfResultTestCase(
            logging.getLogger("hook_logger"),
            "my-test",
            None,
            None,
            None,
            thresholds_to_check,
            reported_metrics,
        )

        with self.assertRaisesRegex(ServerFailure, "threshold check"):
            test_case.run_test()


if __name__ == "__main__":
    unittest.main()
