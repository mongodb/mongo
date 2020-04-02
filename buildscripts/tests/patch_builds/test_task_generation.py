"""Unittests for buildscripts.patch_builds.task_generation.py"""
import unittest

import buildscripts.patch_builds.task_generation as under_test

# pylint: disable=missing-docstring,protected-access,too-many-lines,no-self-use


class TestResmokeCommand(unittest.TestCase):
    def test_basic_command(self):
        run_tests = "run tests"
        test_vars = {}
        timeout_info = under_test.TimeoutInfo.default_timeout()

        commands = under_test.resmoke_commands(run_tests, test_vars, timeout_info)

        # 2 expected command = 1 for setup + 1 for running tests.
        self.assertEqual(2, len(commands))

    def test_with_multiversion(self):
        run_tests = "run tests"
        test_vars = {}
        timeout_info = under_test.TimeoutInfo.default_timeout()

        commands = under_test.resmoke_commands(run_tests, test_vars, timeout_info,
                                               use_multiversion="multiversion")

        # 3 expected command = 1 for setup + 1 for running tests + 1 for multiversion setup.
        self.assertEqual(3, len(commands))

    def test_with_timeout(self):
        run_tests = "run tests"
        test_vars = {}
        timeout_info = under_test.TimeoutInfo.overridden(timeout=5)

        commands = under_test.resmoke_commands(run_tests, test_vars, timeout_info)

        # 3 expected command = 1 for setup + 1 for running tests + 1 for timeout.
        self.assertEqual(3, len(commands))

    def test_with_everything(self):
        run_tests = "run tests"
        test_vars = {}
        timeout_info = under_test.TimeoutInfo.overridden(timeout=5)

        commands = under_test.resmoke_commands(run_tests, test_vars, timeout_info,
                                               use_multiversion="multiversion")

        # 4 expected command = 1 for setup + 1 for running tests + 1 for multiversion setup +
        # 1 for timeout.
        self.assertEqual(4, len(commands))


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
