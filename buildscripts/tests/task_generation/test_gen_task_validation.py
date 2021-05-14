"""Unit tests for gen_task_validation.py"""
import unittest
from unittest.mock import MagicMock

import buildscripts.task_generation.gen_task_validation as under_test

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access


class TestShouldTasksBeGenerated(unittest.TestCase):
    def test_during_first_execution(self):
        task_id = "task_id"
        mock_evg_api = MagicMock()
        mock_evg_api.task_by_id.return_value.execution = 0
        validate_service = under_test.GenTaskValidationService(mock_evg_api)

        self.assertTrue(validate_service.should_task_be_generated(task_id))
        mock_evg_api.task_by_id.assert_called_with(task_id, fetch_all_executions=True)

    def test_after_successful_execution(self):
        task_id = "task_id"
        mock_evg_api = MagicMock()
        task = mock_evg_api.task_by_id.return_value
        task.execution = 1
        task.get_execution.return_value.is_success.return_value = True
        validate_service = under_test.GenTaskValidationService(mock_evg_api)

        self.assertFalse(validate_service.should_task_be_generated(task_id))
        mock_evg_api.task_by_id.assert_called_with(task_id, fetch_all_executions=True)

    def test_after_multiple_successful_execution(self):
        task_id = "task_id"
        mock_evg_api = MagicMock()
        task = mock_evg_api.task_by_id.return_value
        task.execution = 5
        task.get_execution.return_value.is_success.return_value = True
        validate_service = under_test.GenTaskValidationService(mock_evg_api)

        self.assertFalse(validate_service.should_task_be_generated(task_id))
        mock_evg_api.task_by_id.assert_called_with(task_id, fetch_all_executions=True)

    def test_after_failed_execution(self):
        mock_evg_api = MagicMock()
        task_id = "task_id"
        task = mock_evg_api.task_by_id.return_value
        task.execution = 1
        task.get_execution.return_value.is_success.return_value = False
        validate_service = under_test.GenTaskValidationService(mock_evg_api)

        self.assertTrue(validate_service.should_task_be_generated(task_id))
        mock_evg_api.task_by_id.assert_called_with(task_id, fetch_all_executions=True)

    def test_after_multiple_failed_execution(self):
        mock_evg_api = MagicMock()
        task_id = "task_id"
        task = mock_evg_api.task_by_id.return_value
        task.execution = 5
        task.get_execution.return_value.is_success.return_value = False
        validate_service = under_test.GenTaskValidationService(mock_evg_api)

        self.assertTrue(validate_service.should_task_be_generated(task_id))
        mock_evg_api.task_by_id.assert_called_with(task_id, fetch_all_executions=True)
