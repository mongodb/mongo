"""Unit tests for selected_tests_service.py."""
import unittest
from unittest.mock import MagicMock, patch

import buildscripts.patch_builds.selected_tests.selected_tests_service as under_test
from buildscripts.patch_builds.selected_tests.selected_tests_client import TestMappingsResponse, \
    TestMapping, TestFileInstance, TaskMappingsResponse, TaskMapping, TaskMapInstance

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access,no-value-for-parameter


def build_mock_test_mapping(source_file, test_file):
    return TestMapping(branch="branch", project="project", repo="repo", source_file=source_file,
                       source_file_seen_count=5, test_files=[
                           TestFileInstance(name=test_file, test_file_seen_count=3),
                       ])


def build_mock_task_mapping(source_file, task):
    return TaskMapping(branch="branch", project="project", repo="repo", source_file=source_file,
                       source_file_seen_count=5, tasks=[
                           TaskMapInstance(name=task, variant="variant", flip_count=3),
                       ])


class TestFindSelectedTestFiles(unittest.TestCase):
    @patch("os.path.isfile")
    def test_related_files_returned_from_selected_tests_service(self, mock_is_file):
        mock_is_file.return_value = True
        changed_files = {"src/file1.cpp", "src/file2.js"}
        mock_selected_tests_client = MagicMock()
        mock_selected_tests_client.get_test_mappings.return_value = TestMappingsResponse(
            test_mappings=[
                build_mock_test_mapping("src/file1.cpp", "jstests/file-1.js"),
                build_mock_test_mapping("src/file2.cpp", "jstests/file-3.js"),
            ])
        selected_tests = under_test.SelectedTestsService(mock_selected_tests_client)

        related_test_files = selected_tests.find_selected_test_files(changed_files)

        self.assertEqual(related_test_files, {"jstests/file-1.js", "jstests/file-3.js"})

    @patch("os.path.isfile")
    def test_related_files_returned_are_not_valid_test_files(self, mock_is_file):
        mock_is_file.return_value = False
        changed_files = {"src/file1.cpp", "src/file2.js"}
        mock_selected_tests_client = MagicMock()
        mock_selected_tests_client.get_test_mappings.return_value = TestMappingsResponse(
            test_mappings=[
                build_mock_test_mapping("src/file1.cpp", "jstests/file-1.js"),
                build_mock_test_mapping("src/file2.cpp", "jstests/file-3.js"),
            ])
        selected_tests = under_test.SelectedTestsService(mock_selected_tests_client)

        related_test_files = selected_tests.find_selected_test_files(changed_files)

        self.assertEqual(related_test_files, set())

    def test_no_related_files_returned(self):
        changed_files = {"src/file1.cpp", "src/file2.js"}
        mock_selected_tests_client = MagicMock()
        mock_selected_tests_client.get_test_mappings.return_value = TestMappingsResponse(
            test_mappings=[
                build_mock_test_mapping("src/file1.cpp", "jstests/file-1.js"),
                build_mock_test_mapping("src/file2.cpp", "jstests/file-3.js"),
            ])
        selected_tests = under_test.SelectedTestsService(mock_selected_tests_client)

        related_test_files = selected_tests.find_selected_test_files(changed_files)

        self.assertEqual(related_test_files, set())


class TestFindSelectedTasks(unittest.TestCase):
    def test_related_tasks_returned_from_selected_tests_service(self):
        changed_files = {"src/file1.cpp", "src/file2.js"}
        mock_selected_tests_client = MagicMock()
        mock_selected_tests_client.get_task_mappings.return_value = TaskMappingsResponse(
            task_mappings=[
                build_mock_task_mapping("src/file1.cpp", "my_task_1"),
                build_mock_task_mapping("src/file2.cpp", "my_task_2"),
            ])
        selected_tests = under_test.SelectedTestsService(mock_selected_tests_client)

        related_tasks = selected_tests.find_selected_tasks(changed_files)

        self.assertEqual(related_tasks, {"my_task_1", "my_task_2"})
