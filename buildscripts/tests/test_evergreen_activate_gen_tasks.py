"""Unit tests for the generate_resmoke_suite script."""
import unittest

from mock import MagicMock

from buildscripts import evergreen_activate_gen_tasks as under_test

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access
# pylint: disable=too-many-locals,too-many-lines,too-many-public-methods,no-value-for-parameter


def build_mock_task(name, task_id):
    mock_task = MagicMock(display_name=name, task_id=task_id)
    return mock_task


def build_mock_evg_api(mock_task_list):
    mock_build = MagicMock()
    mock_build.get_tasks.return_value = mock_task_list
    mock_evg_api = MagicMock()
    mock_evg_api.build_by_id.return_value = mock_build
    return mock_evg_api


class TestActivateTask(unittest.TestCase):
    def test_task_with_display_name_is_activated(self):
        n_tasks = 5
        mock_task_list = [build_mock_task(f"task_{i}", f"id_{i}") for i in range(n_tasks)]
        mock_evg_api = build_mock_evg_api(mock_task_list)

        under_test.activate_task("build_id", "task_3", mock_evg_api)

        mock_evg_api.configure_task.assert_called_with("id_3", activated=True)

    def test_task_with_no_matching_name(self):
        n_tasks = 5
        mock_task_list = [build_mock_task(f"task_{i}", f"id_{i}") for i in range(n_tasks)]
        mock_evg_api = build_mock_evg_api(mock_task_list)

        under_test.activate_task("build_id", "not_an_existing_task", mock_evg_api)

        mock_evg_api.configure_task.assert_not_called()
