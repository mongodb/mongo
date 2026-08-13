"""Unit tests for failing a run whose build-time test selection file was unusable."""

import unittest
from unittest.mock import MagicMock

from buildscripts.resmokelib.run import TestRunner


class TestFailOnTestSelectionError(unittest.TestCase):
    """A broken selection step must not pass quietly, even though the tests all ran."""

    def setUp(self):
        self.runner = MagicMock(spec=TestRunner)
        self.runner._resmoke_logger = MagicMock()
        self.suite = MagicMock()
        self.suite.get_display_name.return_value = "my_suite"

    def _call(self):
        TestRunner._fail_on_test_selection_error(self.runner, self.suite)

    def test_a_clean_run_is_left_alone(self):
        self.suite.tss_selection_error = None
        self.suite.return_code = 0
        self._call()
        self.assertEqual(0, self.suite.return_code)

    def test_a_selection_error_fails_a_run_whose_tests_all_passed(self):
        self.suite.tss_selection_error = "could not read the file"
        self.suite.return_code = 0
        self._call()
        self.assertNotEqual(0, self.suite.return_code)

    def test_an_existing_failure_is_not_masked(self):
        # A real test failure is a more useful exit code than ours, so keep the higher one.
        self.suite.tss_selection_error = "could not read the file"
        self.suite.return_code = 3
        self._call()
        self.assertEqual(3, self.suite.return_code)

    def test_an_unset_return_code_still_fails(self):
        self.suite.tss_selection_error = "could not read the file"
        self.suite.return_code = None
        self._call()
        self.assertNotEqual(0, self.suite.return_code)


if __name__ == "__main__":
    unittest.main()
