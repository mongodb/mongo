"""Unit tests for timeout.py."""
import unittest

from buildscripts.timeouts import timeout as under_test

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access,no-value-for-parameter


class CalculateTimeoutTest(unittest.TestCase):
    def test_min_timeout(self):
        self.assertEqual(under_test.MIN_TIMEOUT_SECONDS, under_test.calculate_timeout(15, 1))

    def test_over_timeout_by_one_minute(self):
        self.assertEqual(360, under_test.calculate_timeout(301, 1))

    def test_float_runtimes(self):
        self.assertEqual(360, under_test.calculate_timeout(300.14, 1))

    def test_scaling_factor(self):
        avg_runtime = 30
        scaling_factor = 10
        self.assertEqual(avg_runtime * scaling_factor + 60,
                         under_test.calculate_timeout(avg_runtime, scaling_factor))


class TimeoutEstimateTest(unittest.TestCase):
    def test_too_high_a_timeout_raises_errors(self):
        timeout_est = under_test.TimeoutEstimate(
            max_test_runtime=5, expected_task_runtime=under_test.MAX_EXPECTED_TIMEOUT)

        with self.assertRaises(ValueError):
            timeout_est.generate_timeout_cmd(is_patch=True, repeat_factor=1)

    def test_is_specified_should_return_true_when_a_test_runtime_is_specified(self):
        timeout_est = under_test.TimeoutEstimate(max_test_runtime=3.14, expected_task_runtime=None)

        self.assertTrue(timeout_est.is_specified())

    def test_is_specified_should_return_true_when_a_task_runtime_is_specified(self):
        timeout_est = under_test.TimeoutEstimate(max_test_runtime=None, expected_task_runtime=3.14)

        self.assertTrue(timeout_est.is_specified())

    def test_is_specified_should_return_false_when_no_data_is_specified(self):
        timeout_est = under_test.TimeoutEstimate(max_test_runtime=None, expected_task_runtime=None)

        self.assertFalse(timeout_est.is_specified())


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


class TestTimeoutInfo(unittest.TestCase):
    def test_default_timeout(self):
        timeout_info = under_test.TimeoutInfo.default_timeout()

        self.assertIsNone(timeout_info.cmd)

    def test_timeout_only_set(self):
        timeout = 5
        timeout_info = under_test.TimeoutInfo.overridden(timeout=timeout)

        cmd = timeout_info.cmd.as_dict()

        self.assertEqual("timeout.update", cmd["command"])
        self.assertEqual(timeout, cmd["params"]["timeout_secs"])
        self.assertNotIn("exec_timeout_secs", cmd["params"])

    def test_exec_timeout_only_set(self):
        exec_timeout = 5
        timeout_info = under_test.TimeoutInfo.overridden(exec_timeout=exec_timeout)

        cmd = timeout_info.cmd.as_dict()

        self.assertEqual("timeout.update", cmd["command"])
        self.assertEqual(exec_timeout, cmd["params"]["exec_timeout_secs"])
        self.assertNotIn("timeout_secs", cmd["params"])

    def test_both_timeouts_set(self):
        timeout = 3
        exec_timeout = 5
        timeout_info = under_test.TimeoutInfo.overridden(exec_timeout=exec_timeout, timeout=timeout)

        cmd = timeout_info.cmd.as_dict()

        self.assertEqual("timeout.update", cmd["command"])
        self.assertEqual(exec_timeout, cmd["params"]["exec_timeout_secs"])
        self.assertEqual(timeout, cmd["params"]["timeout_secs"])

    def test_override_with_no_values(self):
        with self.assertRaises(ValueError):
            under_test.TimeoutInfo.overridden()
