"""Unit tests for timeout.py."""
import unittest

from buildscripts.task_generation import timeout as under_test

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access,no-value-for-parameter


class CalculateTimeoutTest(unittest.TestCase):
    def test_min_timeout(self):
        self.assertEqual(under_test.MIN_TIMEOUT_SECONDS + under_test.AVG_SETUP_TIME,
                         under_test.calculate_timeout(15, 1))

    def test_over_timeout_by_one_minute(self):
        self.assertEqual(660, under_test.calculate_timeout(301, 1))

    def test_float_runtimes(self):
        self.assertEqual(660, under_test.calculate_timeout(300.14, 1))

    def test_scaling_factor(self):
        scaling_factor = 10
        self.assertEqual(
            under_test.MIN_TIMEOUT_SECONDS * scaling_factor + under_test.AVG_SETUP_TIME,
            under_test.calculate_timeout(30, scaling_factor))


class TimeoutEstimateTest(unittest.TestCase):
    def test_too_high_a_timeout_raises_errors(self):
        timeout_est = under_test.TimeoutEstimate(
            max_test_runtime=5, expected_task_runtime=under_test.MAX_EXPECTED_TIMEOUT)

        with self.assertRaises(ValueError):
            timeout_est.generate_timeout_cmd(is_patch=True, repeat_factor=1)


class TestGenerateTimeoutCmd(unittest.TestCase):
    def test_evg_config_does_not_fails_if_test_timeout_too_high_on_mainline(self):
        timeout = under_test.TimeoutEstimate(max_test_runtime=under_test.MAX_EXPECTED_TIMEOUT + 1,
                                             expected_task_runtime=None)

        time_cmd = timeout.generate_timeout_cmd(is_patch=False, repeat_factor=1)

        self.assertGreater(time_cmd.timeout, under_test.MAX_EXPECTED_TIMEOUT)

    def test_evg_config_does_not_fails_if_task_timeout_too_high_on_mainline(self):
        timeout = under_test.TimeoutEstimate(
            expected_task_runtime=under_test.MAX_EXPECTED_TIMEOUT + 1, max_test_runtime=None)

        time_cmd = timeout.generate_timeout_cmd(is_patch=False, repeat_factor=1)

        self.assertGreater(time_cmd.exec_timeout, under_test.MAX_EXPECTED_TIMEOUT)
