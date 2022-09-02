"""Unit tests for buildscripts/resmokelib/suitesconfig.py."""

import unittest

import mock

from buildscripts.resmokelib import parser
from buildscripts.resmokelib import suitesconfig

parser.set_run_options()

RESMOKELIB = "buildscripts.resmokelib"


class TestSuitesConfig(unittest.TestCase):
    @mock.patch(RESMOKELIB + ".testing.suite.Suite")
    @mock.patch(RESMOKELIB + ".suitesconfig.get_named_suites")
    def test_no_suites_matching_test_kind(self, mock_get_named_suites, mock_suite_class):
        all_suites = ["core", "replica_sets_jscore_passthrough"]
        mock_get_named_suites.return_value = all_suites

        membership_map = suitesconfig.create_test_membership_map(test_kind="nonexistent_test")
        self.assertEqual(membership_map, {})
        self.assertEqual(mock_suite_class.call_count, 2)

    @mock.patch(RESMOKELIB + ".testing.suite.Suite.tests")
    @mock.patch(RESMOKELIB + ".suitesconfig.get_named_suites")
    def test_multiple_suites_matching_single_test_kind(self, mock_get_named_suites,
                                                       mock_suite_get_tests):
        all_suites = ["core", "replica_sets_jscore_passthrough"]
        mock_get_named_suites.return_value = all_suites

        mock_suite_get_tests.__get__ = mock.Mock(return_value=["test1", "test2"])

        membership_map = suitesconfig.create_test_membership_map(test_kind="js_test")
        self.assertEqual(membership_map, dict(test1=all_suites, test2=all_suites))

    @mock.patch(RESMOKELIB + ".testing.suite.Suite.tests")
    @mock.patch(RESMOKELIB + ".suitesconfig.get_named_suites")
    def test_multiple_suites_matching_multiple_test_kinds(self, mock_get_named_suites,
                                                          mock_suite_get_tests):
        all_suites = ["core", "concurrency"]
        mock_get_named_suites.return_value = all_suites

        mock_suite_get_tests.__get__ = mock.Mock(return_value=["test1", "test2"])

        membership_map = suitesconfig.create_test_membership_map(
            test_kind=("fsm_workload_test", "js_test"))
        self.assertEqual(membership_map, dict(test1=all_suites, test2=all_suites))
