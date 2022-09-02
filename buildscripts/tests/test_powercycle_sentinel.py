"""Unit tests for powercycle_sentinel.py."""
import unittest
from datetime import datetime, timezone, timedelta
from unittest.mock import Mock

from evergreen import EvergreenApi, Task

from buildscripts.powercycle_sentinel import watch_tasks, POWERCYCLE_TASK_EXEC_TIMEOUT_SECS


def make_task_mock(evg_api, task_id, start_time, finish_time):
    return Task({
        "task_id": task_id,
        "start_time": start_time,
        "finish_time": finish_time,
    }, evg_api)


class TestWatchTasks(unittest.TestCase):
    """Test watch_tasks."""

    def test_no_long_running_tasks(self):
        evg_api = EvergreenApi()
        task_ids = ["1", "2"]
        now = datetime.now(timezone.utc).isoformat()
        task_1 = make_task_mock(evg_api, task_ids[0], now, now)
        task_2 = make_task_mock(evg_api, task_ids[1], now, now)
        evg_api.task_by_id = Mock(
            side_effect=(lambda task_id: {
                "1": task_1,
                "2": task_2,
            }[task_id]))
        long_running_task_ids = watch_tasks(task_ids, evg_api, 0)
        self.assertEqual([], long_running_task_ids)

    def test_found_long_running_tasks(self):
        evg_api = EvergreenApi()
        task_ids = ["1", "2"]
        exec_timeout_seconds_ago = (datetime.now(timezone.utc) -
                                    timedelta(hours=POWERCYCLE_TASK_EXEC_TIMEOUT_SECS)).isoformat()
        now = datetime.now(timezone.utc).isoformat()
        task_1 = make_task_mock(evg_api, task_ids[0], exec_timeout_seconds_ago, now)
        task_2 = make_task_mock(evg_api, task_ids[1], exec_timeout_seconds_ago, None)
        evg_api.task_by_id = Mock(
            side_effect=(lambda task_id: {
                "1": task_1,
                "2": task_2,
            }[task_id]))
        long_running_task_ids = watch_tasks(task_ids, evg_api, 0)
        self.assertEqual([task_2.task_id], long_running_task_ids)
