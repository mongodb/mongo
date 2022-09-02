"""Unit tests for the validate_mongocryptd script."""

from __future__ import absolute_import

import unittest

from mock import MagicMock, patch

from buildscripts import validate_mongocryptd as under_test

NS = "buildscripts.validate_mongocryptd"


def ns(relative_name):  # pylint: disable=invalid-name
    """Return a full name from a name relative to the test module"s name space."""
    return NS + "." + relative_name


class TestCanValidationBeSkipped(unittest.TestCase):
    def test_non_existing_variant_can_be_skipped(self):
        mock_evg_config = MagicMock()
        mock_evg_config.get_variant.return_value = None
        self.assertTrue(under_test.can_validation_be_skipped(mock_evg_config, "variant"))

    def test_variant_with_no_push_task_can_be_skipped(self):
        mock_evg_config = MagicMock()
        mock_evg_config.get_variant.return_value.task_names = ["task 1", "task 2"]
        self.assertTrue(under_test.can_validation_be_skipped(mock_evg_config, "variant"))

    def test_variant_with_push_task_cannot_be_skipped(self):
        mock_evg_config = MagicMock()
        mock_evg_config.get_variant.return_value.task_names = ["task 1", "push", "task 2"]
        self.assertFalse(under_test.can_validation_be_skipped(mock_evg_config, "variant"))


class TestReadVariableFromYml(unittest.TestCase):
    @patch(ns("open"))
    @patch(ns("yaml"))
    def test_variable_not_in_variables(self, yaml_mock, _):
        mock_nodes = {
            "variables": {},
        }

        yaml_mock.safe_load.return_value = mock_nodes
        self.assertIsNone(under_test.read_variable_from_yml("filename", "variable"))

    @patch(ns("open"))
    @patch(ns("yaml"))
    def test_variable_is_in_variables(self, yaml_mock, _):
        search_key = "var 2"
        expected_value = "value 2"
        mock_nodes = {
            "variables": [
                {"var 1": "value 1"},
                {search_key: expected_value},
                {"var 3": "value 3"},
            ],
        }

        yaml_mock.safe_load.return_value = mock_nodes
        self.assertEqual(expected_value, under_test.read_variable_from_yml("filename", search_key))
