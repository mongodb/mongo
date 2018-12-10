"""Unit tests for the generate_resmoke_suite script."""

from __future__ import absolute_import

import datetime
import math
import os
import unittest
import yaml

from mock import patch, mock_open, call, Mock

from buildscripts import generate_resmoke_suites as grs
from buildscripts.generate_resmoke_suites import render_suite, render_misc_suite, \
    prepare_directory_for_suite

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use

_DATE = datetime.datetime(2018, 7, 15)


class TestTestStats(unittest.TestCase):
    def test_no_hooks(self):
        evg_results = [
            self._make_evg_result("dir/test1.js", 1, 10),
            self._make_evg_result("dir/test2.js", 1, 30),
            self._make_evg_result("dir/test1.js", 2, 25),
        ]
        test_stats = grs.TestStats(evg_results)
        expected_runtimes = [
            ("dir/test2.js", 30),
            ("dir/test1.js", 20),
        ]
        self.assertEqual(expected_runtimes, test_stats.get_tests_runtimes())

    def test_hooks(self):
        evg_results = [
            self._make_evg_result("dir/test1.js", 1, 10),
            self._make_evg_result("dir/test2.js", 1, 30),
            self._make_evg_result("dir/test1.js", 2, 25),
            self._make_evg_result("dir/test3.js", 5, 10),
            self._make_evg_result("test3:CleanEveryN", 10, 30),
            self._make_evg_result("test3:CheckReplDBHash", 10, 35),
        ]
        test_stats = grs.TestStats(evg_results)
        expected_runtimes = [
            ("dir/test3.js", 42.5),
            ("dir/test2.js", 30),
            ("dir/test1.js", 20),
        ]
        self.assertEqual(expected_runtimes, test_stats.get_tests_runtimes())

    @staticmethod
    def _make_evg_result(test_file="dir/test1.js", num_pass=0, duration=0):
        return {
            "test_file": test_file,
            "task_name": "task1",
            "variant": "variant1",
            "distro": "distro1",
            "date": _DATE,
            "num_pass": num_pass,
            "num_fail": 0,
            "avg_duration_pass": duration,
        }


class DivideRemainingTestsAmongSuitesTest(unittest.TestCase):
    @staticmethod
    def generate_tests_runtimes(n_tests):
        tests_runtimes = []
        # Iterating backwards so the list is sorted by descending runtimes
        for idx in range(n_tests - 1, -1, -1):
            name = "test_{0}".format(idx)
            tests_runtimes.append((name, 2 * idx))

        return tests_runtimes

    def test_each_suite_gets_one_test(self):
        suites = [grs.Suite(), grs.Suite(), grs.Suite()]
        tests_runtimes = self.generate_tests_runtimes(3)

        grs.divide_remaining_tests_among_suites(tests_runtimes, suites)

        for suite in suites:
            self.assertEqual(suite.get_test_count(), 1)

    def test_each_suite_gets_at_least_one_test(self):
        suites = [grs.Suite(), grs.Suite(), grs.Suite()]
        tests_runtimes = self.generate_tests_runtimes(5)

        grs.divide_remaining_tests_among_suites(tests_runtimes, suites)

        total_tests = 0
        for suite in suites:
            total_tests += suite.get_test_count()
            self.assertGreaterEqual(suite.get_test_count(), 1)

        self.assertEqual(total_tests, len(tests_runtimes))


class DivideTestsIntoSuitesByMaxtimeTest(unittest.TestCase):
    def test_if_less_total_than_max_only_one_suite_created(self):
        max_time = 20
        tests_runtimes = [
            ("test1", 5),
            ("test2", 4),
            ("test3", 3),
        ]

        suites = grs.divide_tests_into_suites(tests_runtimes, max_time)
        self.assertEqual(len(suites), 1)
        self.assertEqual(suites[0].get_test_count(), 3)
        self.assertEqual(suites[0].get_runtime(), 12)

    def test_if_each_test_should_be_own_suite(self):
        max_time = 5
        tests_runtimes = [
            ("test1", 5),
            ("test2", 4),
            ("test3", 3),
        ]

        suites = grs.divide_tests_into_suites(tests_runtimes, max_time)
        self.assertEqual(len(suites), 3)

    def test_if_test_is_greater_than_max_it_goes_alone(self):
        max_time = 7
        tests_runtimes = [
            ("test1", 15),
            ("test2", 4),
            ("test3", 3),
        ]

        suites = grs.divide_tests_into_suites(tests_runtimes, max_time)
        self.assertEqual(len(suites), 2)
        self.assertEqual(suites[0].get_test_count(), 1)
        self.assertEqual(suites[0].get_runtime(), 15)

    def test_max_sub_suites_options(self):
        max_time = 5
        max_suites = 2
        tests_runtimes = [
            ("test1", 5),
            ("test2", 4),
            ("test3", 3),
            ("test4", 4),
            ("test5", 3),
        ]

        suites = grs.divide_tests_into_suites(tests_runtimes, max_time, max_suites=max_suites)
        self.assertEqual(len(suites), max_suites)
        total_tests = 0
        for suite in suites:
            total_tests += suite.get_test_count()
        self.assertEqual(total_tests, len(tests_runtimes))


class SuiteTest(unittest.TestCase):
    def test_adding_tests_increases_count_and_runtime(self):
        suite = grs.Suite()
        suite.add_test('test1', 10)
        suite.add_test('test2', 12)
        suite.add_test('test3', 7)

        self.assertEqual(suite.get_test_count(), 3)
        self.assertEqual(suite.get_runtime(), 29)


def create_suite(count=3, start=0):
    """ Create a suite with count tests."""
    suite = grs.Suite()
    for i in range(start, start + count):
        suite.add_test('test{}'.format(i), 1)
    return suite


class RenderSuites(unittest.TestCase):
    EXPECTED_FORMAT = """selector:
  excludes:
  - fixed
  roots:
  - test{}
  - test{}
  - test{}
"""

    def _test(self, size):

        suites = [create_suite(start=3 * i) for i in range(size)]
        expected = [
            self.EXPECTED_FORMAT.format(*range(3 * i, 3 * (i + 1))) for i in range(len(suites))
        ]

        m = mock_open(read_data=yaml.dump({'selector': {'roots': [], 'excludes': ['fixed']}}))
        with patch('buildscripts.generate_resmoke_suites.open', m, create=True):
            render_suite(suites, 'suite_name')
        handle = m()

        # The other writes are for the headers.
        self.assertEquals(len(suites) * 2, handle.write.call_count)
        handle.write.assert_has_calls([call(e) for e in expected], any_order=True)
        calls = [
            call(os.path.join(grs.TEST_SUITE_DIR, 'suite_name.yml'), 'r')
            for _ in range(len(suites))
        ]
        m.assert_has_calls(calls, any_order=True)
        filename = os.path.join(grs.CONFIG_DIR, 'suite_name_{{:0{}}}.yml'.format(
            int(math.ceil(math.log10(size)))))
        calls = [call(filename.format(i), 'w') for i in range(size)]
        m.assert_has_calls(calls, any_order=True)

    def test_1_suite(self):
        self._test(1)

    def test_11_suites(self):
        self._test(11)

    def test_101_suites(self):
        self._test(101)


class RenderMiscSuites(unittest.TestCase):
    def test_single_suite(self):

        test_list = ['test{}'.format(i) for i in range(10)]
        m = mock_open(read_data=yaml.dump({'selector': {'roots': []}}))
        with patch('buildscripts.generate_resmoke_suites.open', m, create=True):
            render_misc_suite(test_list, 'suite_name')
        handle = m()

        # The other writes are for the headers.
        self.assertEquals(2, handle.write.call_count)
        handle.write.assert_any_call("""selector:
  exclude_files:
  - test0
  - test1
  - test2
  - test3
  - test4
  - test5
  - test6
  - test7
  - test8
  - test9
  roots: []
""")
        calls = [call(os.path.join(grs.TEST_SUITE_DIR, 'suite_name.yml'), 'r')]
        m.assert_has_calls(calls, any_order=True)
        filename = os.path.join(grs.CONFIG_DIR, 'suite_name_misc.yml')
        calls = [call(filename, 'w')]
        m.assert_has_calls(calls, any_order=True)


class PrepareDirectoryForSuite(unittest.TestCase):
    def test_no_directory(self):
        with patch('buildscripts.generate_resmoke_suites.os') as mock_os:
            mock_os.path.exists.return_value = False
            prepare_directory_for_suite('tmp')

        mock_os.makedirs.assert_called_once_with('tmp')


class GenerateEvgConfigTest(unittest.TestCase):
    @staticmethod
    def generate_mock_suites(count):
        suites = []
        for idx in range(count):
            suite = Mock()
            suite.name = "suite {0}".format(idx)
            suites.append(suite)

        return suites

    @staticmethod
    def generate_mock_options():
        options = Mock()
        options.resmoke_args = "resmoke_args"
        options.run_multiple_jobs = "true"
        options.variant = "buildvariant"
        options.suite = "suite"
        options.task = "suite"

        return options

    def test_evg_config_is_created(self):
        options = self.generate_mock_options()
        suites = self.generate_mock_suites(3)

        config = grs.generate_evg_config(suites, options).to_map()

        self.assertEqual(len(config["tasks"]), len(suites) + 1)
        command1 = config["tasks"][0]["commands"][1]
        self.assertIn(options.resmoke_args, command1["vars"]["resmoke_args"])
        self.assertIn(options.run_multiple_jobs, command1["vars"]["run_multiple_jobs"])
        self.assertEqual("run tests", command1["func"])

    def test_evg_config_is_created_with_diff_task_and_suite(self):
        options = self.generate_mock_options()
        options.task = "task"
        suites = self.generate_mock_suites(3)

        config = grs.generate_evg_config(suites, options).to_map()

        self.assertEqual(len(config["tasks"]), len(suites) + 1)
        display_task = config["buildvariants"][0]["display_tasks"][0]
        self.assertEqual(options.task, display_task["name"])
        self.assertEqual(len(suites) + 2, len(display_task["execution_tasks"]))
        self.assertIn(options.task + "_gen", display_task["execution_tasks"])
        self.assertIn(options.task + "_misc_" + options.variant, display_task["execution_tasks"])

        task = config["tasks"][0]
        self.assertIn(options.variant, task["name"])
        self.assertIn(task["name"], display_task["execution_tasks"])
        self.assertIn(options.suite, task["commands"][1]["vars"]["resmoke_args"])
