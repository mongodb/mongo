#!/usr/bin/env python3
"""Selected Tests service."""
from typing import Set

import inject

from buildscripts.burn_in_tests import is_file_a_test_file
from buildscripts.patch_builds.selected_tests.selected_tests_client import SelectedTestsClient

DEFAULT_THRESHOLD = 0


class SelectedTestsService:
    """A service for interacting with selected tests."""

    @inject.autoparams()
    def __init__(self, selected_tests_client: SelectedTestsClient) -> None:
        """
        Initialize the service.

        :param selected_tests_client: Client to query selected tests.
        """
        self.selected_tests_client = selected_tests_client

    def find_selected_test_files(self, changed_files: Set[str]) -> Set[str]:
        """
        Request related test files from selected-tests service and filter invalid files.

        :param changed_files: Set of changed_files.
        :return: Set of test files returned by selected-tests service that are valid test files.
        """
        test_mappings = self.selected_tests_client.get_test_mappings(DEFAULT_THRESHOLD,
                                                                     changed_files)
        return {
            test_file.name
            for test_mapping in test_mappings.test_mappings for test_file in test_mapping.test_files
            if is_file_a_test_file(test_file.name)
        }

    def find_selected_tasks(self, changed_files: Set[str]) -> Set[str]:
        """
        Request tasks from selected-tests.

        :param changed_files: Set of changed_files.
        :return: Set of tasks returned by selected-tests service that should not be excluded.
        """
        task_mappings = self.selected_tests_client.get_task_mappings(DEFAULT_THRESHOLD,
                                                                     changed_files)
        return {
            task.name
            for task_mapping in task_mappings.task_mappings for task in task_mapping.tasks
        }
