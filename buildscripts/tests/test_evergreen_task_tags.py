"""Unit tests for the evergreen_task_tags script."""

from __future__ import absolute_import

import unittest

from mock import MagicMock

from buildscripts import evergreen_task_tags as ett


def gen_tag_set(prefix, size):
    return {prefix + " " + str(index) for index in range(size)}


class TestGetAllTaskTags(unittest.TestCase):
    def test_with_no_tasks(self):
        evg_config_mock = MagicMock(tasks=[])
        self.assertEqual(0, len(ett.get_all_task_tags(evg_config_mock)))

    def test_with_no_tags(self):
        n_tasks = 5
        task_list_mock = [MagicMock(tags=set()) for _ in range(n_tasks)]
        evg_config_mock = MagicMock(tasks=task_list_mock)
        self.assertEqual(0, len(ett.get_all_task_tags(evg_config_mock)))

    def test_with_some_tags(self):
        task_prefixes = ["b", "a", "q", "v"]
        n_tags = 3
        task_list_mock = [MagicMock(tags=gen_tag_set(prefix, n_tags)) for prefix in task_prefixes]
        evg_config_mock = MagicMock(tasks=task_list_mock)

        tag_list = ett.get_all_task_tags(evg_config_mock)
        self.assertEqual(n_tags * len(task_prefixes), len(tag_list))
        self.assertEqual(sorted(tag_list), tag_list)


class TestGetTasksWithTag(unittest.TestCase):
    def test_with_no_tasks(self):
        evg_config_mock = MagicMock(tasks=[])
        self.assertEqual(0, len(ett.get_tasks_with_tag(evg_config_mock, ["tag"], None)))

    def test_with_no_tags(self):
        n_tasks = 5
        task_list_mock = [MagicMock(tags=set()) for _ in range(n_tasks)]
        evg_config_mock = MagicMock(tasks=task_list_mock)
        self.assertEqual(0, len(ett.get_tasks_with_tag(evg_config_mock, ["tag"], None)))

    def test_with_one_tag_each(self):
        task_prefixes = ["b", "a", "b", "v"]
        n_tags = 3
        task_list_mock = [MagicMock(tags=gen_tag_set(prefix, n_tags)) for prefix in task_prefixes]
        for index, task in enumerate(task_list_mock):
            task.name = "task " + str(index)
        evg_config_mock = MagicMock(tasks=task_list_mock)

        task_list = ett.get_tasks_with_tag(evg_config_mock, ["b 0"], None)
        self.assertIn("task 0", task_list)
        self.assertIn("task 2", task_list)
        self.assertEqual(2, len(task_list))
        self.assertEqual(sorted(task_list), task_list)

    def test_with_two_tags(self):
        task_prefixes = ["b", "a", "b", "v"]
        n_tags = 3
        task_list_mock = [MagicMock(tags=gen_tag_set(prefix, n_tags)) for prefix in task_prefixes]
        for index, task in enumerate(task_list_mock):
            task.name = "task " + str(index)
        evg_config_mock = MagicMock(tasks=task_list_mock)

        task_list = ett.get_tasks_with_tag(evg_config_mock, ["b 0", "b 1"], None)
        self.assertIn("task 0", task_list)
        self.assertIn("task 2", task_list)
        self.assertEqual(2, len(task_list))
        self.assertEqual(sorted(task_list), task_list)

    def test_with_two_tags_no_results(self):
        task_prefixes = ["b", "a", "b", "v"]
        n_tags = 3
        task_list_mock = [MagicMock(tags=gen_tag_set(prefix, n_tags)) for prefix in task_prefixes]
        for index, task in enumerate(task_list_mock):
            task.name = "task " + str(index)
        evg_config_mock = MagicMock(tasks=task_list_mock)

        task_list = ett.get_tasks_with_tag(evg_config_mock, ["b 0", "a 0"], None)
        self.assertEqual(0, len(task_list))

    def test_with_one_filter(self):
        task_prefixes = ["b", "a", "b", "v"]
        n_tags = 3
        task_list_mock = [MagicMock(tags=gen_tag_set(prefix, n_tags)) for prefix in task_prefixes]
        for index, task in enumerate(task_list_mock):
            task.name = "task " + str(index)
        task_list_mock[0].tags = ["b 0"]
        evg_config_mock = MagicMock(tasks=task_list_mock)

        task_list = ett.get_tasks_with_tag(evg_config_mock, ["b 0"], ["b 1"])
        self.assertEqual(1, len(task_list))
        self.assertIn(task_list_mock[0].name, task_list)

    def test_with_two_filter(self):
        task_prefixes = ["b", "a", "b", "v"]
        n_tags = 3
        task_list_mock = [MagicMock(tags=gen_tag_set(prefix, n_tags)) for prefix in task_prefixes]
        for index, task in enumerate(task_list_mock):
            task.name = "task " + str(index)
        task_list_mock[0].tags = ["b 0"]
        evg_config_mock = MagicMock(tasks=task_list_mock)

        task_list = ett.get_tasks_with_tag(evg_config_mock, ["b 0"], ["b 1", "b 0"])
        self.assertEqual(0, len(task_list))


class TestGetAllTasks(unittest.TestCase):
    def test_get_all_tasks_for_empty_variant(self):
        variant = "variant 0"
        evg_config = MagicMock()

        task_list = ett.get_all_tasks(evg_config, variant)
        self.assertEqual(0, len(task_list))

    def test_get_all_tasks_for_variant_with_tasks(self):
        variant = "variant 0"
        evg_config = MagicMock()

        task_list = ett.get_all_tasks(evg_config, variant)
        self.assertEqual(evg_config.get_variant.return_value.task_names, task_list)
        evg_config.get_variant.assert_called_with(variant)
