"""Unit tests for the evergreen_task_timeout script."""
import unittest
from datetime import timedelta
from unittest.mock import MagicMock

import buildscripts.evergreen_task_timeout as under_test
from buildscripts.ciconfig.evergreen import EvergreenProjectConfig
from buildscripts.timeouts.timeout_service import TimeoutService

# pylint: disable=missing-docstring,no-self-use,invalid-name,protected-access


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
    def test_timeout_used_if_specified(self):
        mock_timeout_overrides = under_test.TimeoutOverrides(overrides={})
        orchestrator = under_test.TaskTimeoutOrchestrator(
            timeout_service=MagicMock(spec_set=TimeoutService),
            timeout_overrides=mock_timeout_overrides,
            evg_project_config=MagicMock(spec_set=EvergreenProjectConfig))
        timeout = timedelta(seconds=42)
        self.assertEqual(
            orchestrator.determine_exec_timeout("task_name", "variant", None, timeout), timeout)

    def test_default_is_returned_with_no_timeout(self):
        mock_timeout_overrides = under_test.TimeoutOverrides(overrides={})
        orchestrator = under_test.TaskTimeoutOrchestrator(
            timeout_service=MagicMock(spec_set=TimeoutService),
            timeout_overrides=mock_timeout_overrides,
            evg_project_config=MagicMock(spec_set=EvergreenProjectConfig))
        self.assertEqual(
            orchestrator.determine_exec_timeout("task_name", "variant"),
            under_test.DEFAULT_NON_REQUIRED_BUILD_TIMEOUT)

    def test_default_is_returned_with_timeout_at_zero(self):
        mock_timeout_overrides = under_test.TimeoutOverrides(overrides={})
        orchestrator = under_test.TaskTimeoutOrchestrator(
            timeout_service=MagicMock(spec_set=TimeoutService),
            timeout_overrides=mock_timeout_overrides,
            evg_project_config=MagicMock(spec_set=EvergreenProjectConfig))
        self.assertEqual(
            orchestrator.determine_exec_timeout("task_name", "variant", timedelta(seconds=0)),
            under_test.DEFAULT_NON_REQUIRED_BUILD_TIMEOUT)

    def test_default_required_returned_on_required_variants(self):
        mock_timeout_overrides = under_test.TimeoutOverrides(overrides={})
        orchestrator = under_test.TaskTimeoutOrchestrator(
            timeout_service=MagicMock(spec_set=TimeoutService),
            timeout_overrides=mock_timeout_overrides,
            evg_project_config=MagicMock(spec_set=EvergreenProjectConfig))
        self.assertEqual(
            orchestrator.determine_exec_timeout("task_name", "variant-required"),
            under_test.DEFAULT_REQUIRED_BUILD_TIMEOUT)

    def test_task_specific_timeout(self):
        mock_timeout_overrides = under_test.TimeoutOverrides(
            overrides={"linux-64-debug": [{"task": "auth", "exec_timeout": 60}]})
        orchestrator = under_test.TaskTimeoutOrchestrator(
            timeout_service=MagicMock(spec_set=TimeoutService),
            timeout_overrides=mock_timeout_overrides,
            evg_project_config=MagicMock(spec_set=EvergreenProjectConfig))
        self.assertEqual(
            orchestrator.determine_exec_timeout("auth", "linux-64-debug"), timedelta(minutes=60))

    def test_commit_queue_items_use_commit_queue_timeout(self):
        mock_timeout_overrides = under_test.TimeoutOverrides(overrides={})
        orchestrator = under_test.TaskTimeoutOrchestrator(
            timeout_service=MagicMock(spec_set=TimeoutService),
            timeout_overrides=mock_timeout_overrides,
            evg_project_config=MagicMock(spec_set=EvergreenProjectConfig))
        timeout = orchestrator.determine_exec_timeout("auth", "variant",
                                                      evg_alias=under_test.COMMIT_QUEUE_ALIAS)
        self.assertEqual(timeout, under_test.COMMIT_QUEUE_TIMEOUT)

    def test_use_idle_timeout_if_greater_than_exec_timeout(self):
        mock_timeout_overrides = under_test.TimeoutOverrides(overrides={})
        orchestrator = under_test.TaskTimeoutOrchestrator(
            timeout_service=MagicMock(spec_set=TimeoutService),
            timeout_overrides=mock_timeout_overrides,
            evg_project_config=MagicMock(spec_set=EvergreenProjectConfig))
        idle_timeout = timedelta(hours=2)
        exec_timeout = timedelta(minutes=10)
        timeout = orchestrator.determine_exec_timeout(
            "task_name", "variant", idle_timeout=idle_timeout, exec_timeout=exec_timeout)

        self.assertEqual(timeout, idle_timeout)


class TestDetermineIdleTimeout(unittest.TestCase):
    def test_timeout_used_if_specified(self):
        mock_timeout_overrides = under_test.TimeoutOverrides(overrides={})
        orchestrator = under_test.TaskTimeoutOrchestrator(
            timeout_service=MagicMock(spec_set=TimeoutService),
            timeout_overrides=mock_timeout_overrides,
            evg_project_config=MagicMock(spec_set=EvergreenProjectConfig))
        timeout = timedelta(seconds=42)
        self.assertEqual(
            orchestrator.determine_idle_timeout("task_name", "variant", timeout), timeout)

    def test_default_is_returned_with_no_timeout(self):
        mock_timeout_overrides = under_test.TimeoutOverrides(overrides={})
        orchestrator = under_test.TaskTimeoutOrchestrator(
            timeout_service=MagicMock(spec_set=TimeoutService),
            timeout_overrides=mock_timeout_overrides,
            evg_project_config=MagicMock(spec_set=EvergreenProjectConfig))
        self.assertIsNone(orchestrator.determine_idle_timeout("task_name", "variant"))

    def test_task_specific_timeout(self):
        mock_timeout_overrides = under_test.TimeoutOverrides(
            overrides={"linux-64-debug": [{"task": "auth", "idle_timeout": 60}]})
        orchestrator = under_test.TaskTimeoutOrchestrator(
            timeout_service=MagicMock(spec_set=TimeoutService),
            timeout_overrides=mock_timeout_overrides,
            evg_project_config=MagicMock(spec_set=EvergreenProjectConfig))
        self.assertEqual(
            orchestrator.determine_idle_timeout("auth", "linux-64-debug"), timedelta(minutes=60))
