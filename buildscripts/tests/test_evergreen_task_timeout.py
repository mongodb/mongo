"""Unit tests for the evergreen_task_timeout script."""
import unittest
from datetime import timedelta
from unittest.mock import MagicMock

import buildscripts.evergreen_task_timeout as under_test
from buildscripts.ciconfig.evergreen import EvergreenProjectConfig
from buildscripts.timeouts.timeout_service import TimeoutService

# pylint: disable=invalid-name


class TestTimeoutOverride(unittest.TestCase):
    def test_exec_timeout_should_be_settable(self):
        timeout_override = under_test.TimeoutOverride(task="my task", exec_timeout=42)

        timeout = timeout_override.get_exec_timeout()

        self.assertIsNotNone(timeout)
        self.assertEqual(42 * 60, timeout.total_seconds())

    def test_exec_timeout_should_default_to_none(self):
        timeout_override = under_test.TimeoutOverride(task="my task")

        timeout = timeout_override.get_exec_timeout()

        self.assertIsNone(timeout)

    def test_idle_timeout_should_be_settable(self):
        timeout_override = under_test.TimeoutOverride(task="my task", idle_timeout=42)

        timeout = timeout_override.get_idle_timeout()

        self.assertIsNotNone(timeout)
        self.assertEqual(42 * 60, timeout.total_seconds())

    def test_idle_timeout_should_default_to_none(self):
        timeout_override = under_test.TimeoutOverride(task="my task")

        timeout = timeout_override.get_idle_timeout()

        self.assertIsNone(timeout)


class TestTimeoutOverrides(unittest.TestCase):
    def test_looking_up_a_non_existing_override_should_return_none(self):
        timeout_overrides = under_test.TimeoutOverrides(overrides={})

        self.assertIsNone(timeout_overrides.lookup_exec_override("bv", "task"))
        self.assertIsNone(timeout_overrides.lookup_idle_override("bv", "task"))

    def test_looking_up_a_duplicate_override_should_raise_error(self):
        timeout_overrides = under_test.TimeoutOverrides(
            overrides={
                "bv": [{
                    "task": "task_name",
                    "exec_timeout": 42,
                    "idle_timeout": 10,
                }, {
                    "task": "task_name",
                    "exec_timeout": 314,
                    "idle_timeout": 20,
                }]
            })

        with self.assertRaises(ValueError):
            self.assertIsNone(timeout_overrides.lookup_exec_override("bv", "task_name"))

        with self.assertRaises(ValueError):
            self.assertIsNone(timeout_overrides.lookup_idle_override("bv", "task_name"))

    def test_looking_up_an_exec_override_should_work(self):
        timeout_overrides = under_test.TimeoutOverrides(
            overrides={
                "bv": [
                    {
                        "task": "another_task",
                        "exec_timeout": 314,
                        "idle_timeout": 20,
                    },
                    {
                        "task": "task_name",
                        "exec_timeout": 42,
                    },
                ]
            })

        self.assertEqual(42 * 60,
                         timeout_overrides.lookup_exec_override("bv", "task_name").total_seconds())

    def test_looking_up_an_idle_override_should_work(self):
        timeout_overrides = under_test.TimeoutOverrides(
            overrides={
                "bv": [
                    {
                        "task": "another_task",
                        "exec_timeout": 314,
                        "idle_timeout": 20,
                    },
                    {
                        "task": "task_name",
                        "idle_timeout": 10,
                    },
                ]
            })

        self.assertEqual(10 * 60,
                         timeout_overrides.lookup_idle_override("bv", "task_name").total_seconds())


class TestDetermineExecTimeout(unittest.TestCase):
    def _validate_exec_timeout(self, idle_timeout, exec_timeout, historic_timeout, evg_alias,
                               build_variant, timeout_override, expected_timeout):
        task_name = "task_name"
        variant = build_variant
        overrides = {}
        if timeout_override is not None:
            overrides[variant] = [{"task": task_name, "exec_timeout": timeout_override}]

        mock_timeout_overrides = under_test.TimeoutOverrides(overrides=overrides)

        orchestrator = under_test.TaskTimeoutOrchestrator(
            timeout_service=MagicMock(spec_set=TimeoutService),
            timeout_overrides=mock_timeout_overrides,
            evg_project_config=MagicMock(spec_set=EvergreenProjectConfig))

        actual_timeout = orchestrator.determine_exec_timeout(
            task_name, variant, idle_timeout, exec_timeout, evg_alias, historic_timeout)

        self.assertEqual(actual_timeout, expected_timeout)

    def test_timeout_used_if_specified(self):
        self._validate_exec_timeout(idle_timeout=None, exec_timeout=timedelta(seconds=42),
                                    historic_timeout=None, evg_alias=None, build_variant="variant",
                                    timeout_override=None, expected_timeout=timedelta(seconds=42))

    def test_default_is_returned_with_no_timeout(self):
        self._validate_exec_timeout(idle_timeout=None, exec_timeout=None, historic_timeout=None,
                                    evg_alias=None, build_variant="variant", timeout_override=None,
                                    expected_timeout=under_test.DEFAULT_NON_REQUIRED_BUILD_TIMEOUT)

    def test_default_is_returned_with_timeout_at_zero(self):
        self._validate_exec_timeout(idle_timeout=None, exec_timeout=timedelta(seconds=0),
                                    historic_timeout=None, evg_alias=None, build_variant="variant",
                                    timeout_override=None,
                                    expected_timeout=under_test.DEFAULT_NON_REQUIRED_BUILD_TIMEOUT)

    def test_default_required_returned_on_required_variants(self):
        self._validate_exec_timeout(idle_timeout=None, exec_timeout=None, historic_timeout=None,
                                    evg_alias=None, build_variant="variant-required",
                                    timeout_override=None,
                                    expected_timeout=under_test.DEFAULT_REQUIRED_BUILD_TIMEOUT)

    def test_override_on_required_should_use_override(self):
        self._validate_exec_timeout(idle_timeout=None, exec_timeout=None, historic_timeout=None,
                                    evg_alias=None, build_variant="variant-required",
                                    timeout_override=3 * 60,
                                    expected_timeout=timedelta(minutes=3 * 60))

    def test_task_specific_timeout(self):
        self._validate_exec_timeout(idle_timeout=None, exec_timeout=timedelta(seconds=0),
                                    historic_timeout=None, evg_alias=None, build_variant="variant",
                                    timeout_override=60, expected_timeout=timedelta(minutes=60))

    def test_commit_queue_items_use_commit_queue_timeout(self):
        self._validate_exec_timeout(idle_timeout=None, exec_timeout=None, historic_timeout=None,
                                    evg_alias=under_test.COMMIT_QUEUE_ALIAS,
                                    build_variant="variant", timeout_override=None,
                                    expected_timeout=under_test.COMMIT_QUEUE_TIMEOUT)

    def test_use_idle_timeout_if_greater_than_exec_timeout(self):
        self._validate_exec_timeout(
            idle_timeout=timedelta(hours=2), exec_timeout=timedelta(minutes=10),
            historic_timeout=None, evg_alias=None, build_variant="variant", timeout_override=None,
            expected_timeout=timedelta(hours=2))

    def test_historic_timeout_should_be_used_if_given(self):
        self._validate_exec_timeout(idle_timeout=None, exec_timeout=None,
                                    historic_timeout=timedelta(minutes=15), evg_alias=None,
                                    build_variant="variant", timeout_override=None,
                                    expected_timeout=timedelta(minutes=15))

    def test_commit_queue_should_override_historic_timeouts(self):
        self._validate_exec_timeout(
            idle_timeout=None, exec_timeout=None, historic_timeout=timedelta(minutes=15),
            evg_alias=under_test.COMMIT_QUEUE_ALIAS, build_variant="variant", timeout_override=None,
            expected_timeout=under_test.COMMIT_QUEUE_TIMEOUT)

    def test_override_should_override_historic_timeouts(self):
        self._validate_exec_timeout(idle_timeout=None, exec_timeout=None,
                                    historic_timeout=timedelta(minutes=15), evg_alias=None,
                                    build_variant="variant", timeout_override=33,
                                    expected_timeout=timedelta(minutes=33))

    def test_historic_timeout_should_not_be_overridden_by_required_bv(self):
        self._validate_exec_timeout(idle_timeout=None, exec_timeout=None,
                                    historic_timeout=timedelta(minutes=15), evg_alias=None,
                                    build_variant="variant-required", timeout_override=None,
                                    expected_timeout=timedelta(minutes=15))

    def test_historic_timeout_should_not_be_increase_required_bv_timeout(self):
        self._validate_exec_timeout(
            idle_timeout=None, exec_timeout=None,
            historic_timeout=under_test.DEFAULT_REQUIRED_BUILD_TIMEOUT + timedelta(minutes=30),
            evg_alias=None, build_variant="variant-required", timeout_override=None,
            expected_timeout=under_test.DEFAULT_REQUIRED_BUILD_TIMEOUT)


class TestDetermineIdleTimeout(unittest.TestCase):
    def _validate_idle_timeout(self, idle_timeout, historic_timeout, build_variant,
                               timeout_override, expected_timeout):
        task_name = "task_name"
        overrides = {}
        if timeout_override is not None:
            overrides[build_variant] = [{"task": task_name, "idle_timeout": timeout_override}]

        mock_timeout_overrides = under_test.TimeoutOverrides(overrides=overrides)

        orchestrator = under_test.TaskTimeoutOrchestrator(
            timeout_service=MagicMock(spec_set=TimeoutService),
            timeout_overrides=mock_timeout_overrides,
            evg_project_config=MagicMock(spec_set=EvergreenProjectConfig))

        actual_timeout = orchestrator.determine_idle_timeout(task_name, build_variant, idle_timeout,
                                                             historic_timeout)

        self.assertEqual(actual_timeout, expected_timeout)

    def test_timeout_used_if_specified(self):
        self._validate_idle_timeout(
            idle_timeout=timedelta(seconds=42),
            historic_timeout=None,
            build_variant="variant",
            timeout_override=None,
            expected_timeout=timedelta(seconds=42),
        )

    def test_default_is_returned_with_no_timeout(self):
        self._validate_idle_timeout(
            idle_timeout=None,
            historic_timeout=None,
            build_variant="variant",
            timeout_override=None,
            expected_timeout=None,
        )

    def test_task_specific_timeout(self):
        self._validate_idle_timeout(
            idle_timeout=None,
            historic_timeout=None,
            build_variant="variant",
            timeout_override=60,
            expected_timeout=timedelta(minutes=60),
        )

    def test_historic_timeout_should_be_used_if_given(self):
        self._validate_idle_timeout(idle_timeout=None, historic_timeout=timedelta(minutes=15),
                                    build_variant="variant", timeout_override=None,
                                    expected_timeout=timedelta(minutes=15))

    def test_override_should_override_historic_timeout(self):
        self._validate_idle_timeout(idle_timeout=None, historic_timeout=timedelta(minutes=15),
                                    build_variant="variant", timeout_override=30,
                                    expected_timeout=timedelta(minutes=30))
