"""Unit tests for the generate_resmoke_suite script."""

import datetime
import math
import os
import unittest

import requests
import yaml

from mock import patch, mock_open, call, Mock, MagicMock

from buildscripts import evergreen_generate_resmoke_tasks as grt
from buildscripts.evergreen_generate_resmoke_tasks import render_suite, render_misc_suite, \
    prepare_directory_for_suite, remove_gen_suffix

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access

_DATE = datetime.datetime(2018, 7, 15)

NS = "buildscripts.evergreen_generate_resmoke_tasks"


def ns(relative_name):  # pylint: disable-invalid-name
    """Return a full name from a name relative to the test module"s name space."""
    return NS + "." + relative_name


class TestHelperMethods(unittest.TestCase):
    def test_removes_gen_suffix(self):
        input_task_name = "sharding_auth_audit_gen"
        self.assertEqual("sharding_auth_audit", remove_gen_suffix(input_task_name))

    def test_doesnt_remove_non_gen_suffix(self):
        input_task_name = "sharded_multi_stmt_txn_jscore_passthrough"
        self.assertEqual("sharded_multi_stmt_txn_jscore_passthrough", remove_gen_suffix(input_task_name))


class TestTestStats(unittest.TestCase):
    def test_no_hooks(self):
        evg_results = [
            self._make_evg_result("dir/test1.js", 1, 10),
            self._make_evg_result("dir/test2.js", 1, 30),
            self._make_evg_result("dir/test1.js", 2, 25),
        ]
        test_stats = grt.TestStats(evg_results)
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
        test_stats = grt.TestStats(evg_results)
        expected_runtimes = [
            ("dir/test3.js", 42.5),
            ("dir/test2.js", 30),
            ("dir/test1.js", 20),
        ]
        self.assertEqual(expected_runtimes, test_stats.get_tests_runtimes())

    @staticmethod
    def _make_evg_result(test_file="dir/test1.js", num_pass=0, duration=0):
        return Mock(
            test_file=test_file,
            task_name="task1",
            variant="variant1",
            distro="distro1",
            date=_DATE,
            num_pass=num_pass,
            num_fail=0,
            avg_duration_pass=duration,
        )


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
        suites = [grt.Suite(), grt.Suite(), grt.Suite()]
        tests_runtimes = self.generate_tests_runtimes(3)

        grt.divide_remaining_tests_among_suites(tests_runtimes, suites)

        for suite in suites:
            self.assertEqual(suite.get_test_count(), 1)

    def test_each_suite_gets_at_least_one_test(self):
        suites = [grt.Suite(), grt.Suite(), grt.Suite()]
        tests_runtimes = self.generate_tests_runtimes(5)

        grt.divide_remaining_tests_among_suites(tests_runtimes, suites)

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

        suites = grt.divide_tests_into_suites(tests_runtimes, max_time)
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

        suites = grt.divide_tests_into_suites(tests_runtimes, max_time)
        self.assertEqual(len(suites), 3)

    def test_if_test_is_greater_than_max_it_goes_alone(self):
        max_time = 7
        tests_runtimes = [
            ("test1", 15),
            ("test2", 4),
            ("test3", 3),
        ]

        suites = grt.divide_tests_into_suites(tests_runtimes, max_time)
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

        suites = grt.divide_tests_into_suites(tests_runtimes, max_time, max_suites=max_suites)
        self.assertEqual(len(suites), max_suites)
        total_tests = 0
        for suite in suites:
            total_tests += suite.get_test_count()
        self.assertEqual(total_tests, len(tests_runtimes))


class SuiteTest(unittest.TestCase):
    def test_adding_tests_increases_count_and_runtime(self):
        suite = grt.Suite()
        suite.add_test("test1", 10)
        suite.add_test("test2", 12)
        suite.add_test("test3", 7)

        self.assertEqual(suite.get_test_count(), 3)
        self.assertEqual(suite.get_runtime(), 29)
        self.assertTrue(suite.should_overwrite_timeout())

    def test_suites_without_full_runtime_history_should_not_be_overridden(self):
        suite = grt.Suite()
        suite.add_test("test1", 10)
        suite.add_test("test2", 0)
        suite.add_test("test3", 7)

        self.assertFalse(suite.should_overwrite_timeout())


def create_suite(count=3, start=0):
    """ Create a suite with count tests."""
    suite = grt.Suite()
    for i in range(start, start + count):
        suite.add_test("test{}".format(i), 1)
    return suite


class UpdateSuiteConfigTest(unittest.TestCase):
    def test_roots_are_updated(self):
        config = {"selector": {}}

        updated_config = grt.update_suite_config(config, "root value")
        self.assertEqual("root value", updated_config["selector"]["roots"])

    def test_excluded_files_not_included_if_not_specified(self):
        config = {"selector": {"excluded_files": "files to exclude"}}

        updated_config = grt.update_suite_config(config, excludes=None)
        self.assertNotIn("exclude_files", updated_config["selector"])

    def test_excluded_files_added_to_misc(self):
        config = {"selector": {}}

        updated_config = grt.update_suite_config(config, excludes="files to exclude")
        self.assertEqual("files to exclude", updated_config["selector"]["exclude_files"])

    def test_excluded_files_extended_in_misc(self):
        config = {"selector": {"exclude_files": ["file 0", "file 1"]}}

        updated_config = grt.update_suite_config(config, excludes=["file 2", "file 3"])
        self.assertEqual(4, len(updated_config["selector"]["exclude_files"]))
        for exclude in ["file 0", "file 1", "file 2", "file 3"]:
            self.assertIn(exclude, updated_config["selector"]["exclude_files"])


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
            self.EXPECTED_FORMAT.format(*list(range(3 * i, 3 * (i + 1))))
            for i in range(len(suites))
        ]

        m = mock_open(read_data=yaml.dump({"selector": {"roots": [], "excludes": ["fixed"]}}))
        with patch(ns("open"), m, create=True):
            render_suite(suites, "suite_name")
        handle = m()

        # The other writes are for the headers.
        self.assertEqual(len(suites) * 2, handle.write.call_count)
        handle.write.assert_has_calls([call(e) for e in expected], any_order=True)
        calls = [
            call(os.path.join(grt.TEST_SUITE_DIR, "suite_name.yml"), "r")
            for _ in range(len(suites))
        ]
        m.assert_has_calls(calls, any_order=True)
        filename = os.path.join(grt.CONFIG_DIR, "suite_name_{{:0{}}}.yml".format(
            int(math.ceil(math.log10(size)))))
        calls = [call(filename.format(i), "w") for i in range(size)]
        m.assert_has_calls(calls, any_order=True)

    def test_1_suite(self):
        self._test(1)

    def test_11_suites(self):
        self._test(11)

    def test_101_suites(self):
        self._test(101)


class RenderMiscSuites(unittest.TestCase):
    def test_single_suite(self):

        test_list = ["test{}".format(i) for i in range(10)]
        m = mock_open(read_data=yaml.dump({"selector": {"roots": []}}))
        with patch(ns("open"), m, create=True):
            render_misc_suite(test_list, "suite_name")
        handle = m()

        # The other writes are for the headers.
        self.assertEqual(2, handle.write.call_count)
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
        calls = [call(os.path.join(grt.TEST_SUITE_DIR, "suite_name.yml"), "r")]
        m.assert_has_calls(calls, any_order=True)
        filename = os.path.join(grt.CONFIG_DIR, "suite_name_misc.yml")
        calls = [call(filename, "w")]
        m.assert_has_calls(calls, any_order=True)


class PrepareDirectoryForSuite(unittest.TestCase):
    def test_no_directory(self):
        with patch(ns("os")) as mock_os:
            mock_os.path.exists.return_value = False
            prepare_directory_for_suite("tmp")

        mock_os.makedirs.assert_called_once_with("tmp")


class CalculateTimeoutTest(unittest.TestCase):
    def test_min_timeout(self):
        self.assertEqual(grt.MIN_TIMEOUT_SECONDS, grt.calculate_timeout(15, 1))

    def test_over_timeout_by_one_minute(self):
        self.assertEqual(360, grt.calculate_timeout(301, 1))

    def test_float_runtimes(self):
        self.assertEqual(360, grt.calculate_timeout(300.14, 1))

    def test_scaling_factor(self):
        scaling_factor = 10
        self.assertEqual(grt.MIN_TIMEOUT_SECONDS * scaling_factor,
                         grt.calculate_timeout(30, scaling_factor))


class EvergreenConfigGeneratorTest(unittest.TestCase):
    @staticmethod
    def generate_mock_suites(count):
        suites = []
        for idx in range(count):
            suite = Mock()
            suite.name = "suite {0}".format(idx)
            suite.max_runtime = 5.28
            suite.get_runtime = lambda: 100.874
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
        options.use_default_timeouts = False
        options.use_large_distro = None
        options.use_multiversion = False
        options.is_patch = True
        options.repeat_suites = 1

        return options

    def test_evg_config_is_created(self):
        options = self.generate_mock_options()
        suites = self.generate_mock_suites(3)

        config = grt.EvergreenConfigGenerator(suites, options, Mock()).generate_config().to_map()

        self.assertEqual(len(config["tasks"]), len(suites) + 1)
        command1 = config["tasks"][0]["commands"][2]
        self.assertIn(options.resmoke_args, command1["vars"]["resmoke_args"])
        self.assertIn(" --originSuite=suite", command1["vars"]["resmoke_args"])
        self.assertIn(options.run_multiple_jobs, command1["vars"]["run_multiple_jobs"])
        self.assertEqual("run generated tests", command1["func"])

    def test_evg_config_is_created_with_diff_task_and_suite(self):
        options = self.generate_mock_options()
        options.task = "task"
        suites = self.generate_mock_suites(3)

        config = grt.EvergreenConfigGenerator(suites, options, Mock()).generate_config().to_map()

        self.assertEqual(len(config["tasks"]), len(suites) + 1)
        display_task = config["buildvariants"][0]["display_tasks"][0]
        self.assertEqual(options.task, display_task["name"])
        self.assertEqual(len(suites) + 2, len(display_task["execution_tasks"]))
        self.assertIn(options.task + "_gen", display_task["execution_tasks"])
        self.assertIn(options.task + "_misc_" + options.variant, display_task["execution_tasks"])

        task = config["tasks"][0]
        self.assertIn(options.variant, task["name"])
        self.assertIn(task["name"], display_task["execution_tasks"])
        self.assertIn(options.suite, task["commands"][2]["vars"]["resmoke_args"])

    def test_evg_config_can_use_large_distro(self):
        options = self.generate_mock_options()
        options.use_large_distro = "true"
        options.large_distro_name = "large distro name"

        suites = self.generate_mock_suites(3)

        config = grt.EvergreenConfigGenerator(suites, options, Mock()).generate_config().to_map()

        self.assertEqual(len(config["tasks"]), len(suites) + 1)
        self.assertEqual(options.large_distro_name,
                         config["buildvariants"][0]["tasks"][0]["distros"][0])

    def test_selecting_tasks(self):
        is_task_dependency = grt.EvergreenConfigGenerator._is_task_dependency
        self.assertFalse(is_task_dependency("sharding", "sharding"))
        self.assertFalse(is_task_dependency("sharding", "other_task"))
        self.assertFalse(is_task_dependency("sharding", "sharding_gen"))

        self.assertTrue(is_task_dependency("sharding", "sharding_0"))
        self.assertTrue(is_task_dependency("sharding", "sharding_314"))
        self.assertTrue(is_task_dependency("sharding", "sharding_misc"))

    def test_get_tasks_depends_on(self):
        options = self.generate_mock_options()
        suites = self.generate_mock_suites(3)

        cfg_generator = grt.EvergreenConfigGenerator(suites, options, Mock())
        cfg_generator.build_tasks = [
            Mock(display_name="sharding_gen"),
            Mock(display_name="sharding_0"),
            Mock(display_name="other_task"),
            Mock(display_name="other_task_2"),
            Mock(display_name="sharding_1"),
            Mock(display_name="compile"),
            Mock(display_name="sharding_misc"),
        ]

        dependent_tasks = cfg_generator._get_tasks_for_depends_on("sharding")
        self.assertEqual(3, len(dependent_tasks))
        self.assertIn("sharding_0", dependent_tasks)
        self.assertIn("sharding_1", dependent_tasks)
        self.assertIn("sharding_misc", dependent_tasks)

    def test_specified_dependencies_are_added(self):
        options = self.generate_mock_options()
        options.depends_on = ["sharding"]
        options.is_patch = False
        suites = self.generate_mock_suites(3)

        cfg_generator = grt.EvergreenConfigGenerator(suites, options, Mock())
        cfg_generator.build_tasks = [
            Mock(display_name="sharding_gen"),
            Mock(display_name="sharding_0"),
            Mock(display_name="other_task"),
            Mock(display_name="other_task_2"),
            Mock(display_name="sharding_1"),
            Mock(display_name="compile"),
            Mock(display_name="sharding_misc"),
        ]

        cfg_mock = Mock()
        cfg_generator._add_dependencies(cfg_mock)
        self.assertEqual(4, cfg_mock.dependency.call_count)

    def test_evg_config_has_timeouts_for_repeated_suites(self):
        options = self.generate_mock_options()
        options.repeat_suites = 5
        suites = self.generate_mock_suites(3)

        config = grt.EvergreenConfigGenerator(suites, options, Mock()).generate_config().to_map()

        self.assertEqual(len(config["tasks"]), len(suites) + 1)
        command1 = config["tasks"][0]["commands"][2]
        self.assertIn(" --repeatSuites=5 ", command1["vars"]["resmoke_args"])
        self.assertIn(options.resmoke_args, command1["vars"]["resmoke_args"])
        timeout_cmd = config["tasks"][0]["commands"][0]
        self.assertEqual("timeout.update", timeout_cmd["command"])
        expected_timeout = grt.calculate_timeout(suites[0].max_runtime, 3) * 5
        self.assertEqual(expected_timeout, timeout_cmd["params"]["timeout_secs"])
        expected_exec_timeout = grt.calculate_timeout(suites[0].get_runtime(), 3) * 5
        self.assertEqual(expected_exec_timeout, timeout_cmd["params"]["exec_timeout_secs"])

    def test_suites_without_enough_info_should_not_include_timeouts(self):
        suite_without_timing_info = 1
        options = self.generate_mock_options()
        suites = self.generate_mock_suites(3)
        suites[suite_without_timing_info].should_overwrite_timeout.return_value = False

        config = grt.EvergreenConfigGenerator(suites, options, Mock()).generate_config().to_map()

        timeout_cmd = config["tasks"][suite_without_timing_info]["commands"][0]
        self.assertNotIn("command", timeout_cmd)
        self.assertEqual("do setup", timeout_cmd["func"])

    def test_timeout_info_not_included_if_use_default_timeouts_set(self):
        suite_without_timing_info = 1
        options = self.generate_mock_options()
        suites = self.generate_mock_suites(3)
        options.use_default_timeouts = True

        config = grt.EvergreenConfigGenerator(suites, options, Mock()).generate_config().to_map()

        timeout_cmd = config["tasks"][suite_without_timing_info]["commands"][0]
        self.assertNotIn("command", timeout_cmd)
        self.assertEqual("do setup", timeout_cmd["func"])


class NormalizeTestNameTest(unittest.TestCase):
    def test_unix_names(self):
        self.assertEqual("/home/user/test.js", grt.normalize_test_name("/home/user/test.js"))

    def test_windows_names(self):
        self.assertEqual("/home/user/test.js", grt.normalize_test_name("\\home\\user\\test.js"))


class MainTest(unittest.TestCase):
    @staticmethod
    def get_mock_options():
        options = Mock()
        options.target_resmoke_time = 10
        options.fallback_num_sub_suites = 2
        return options

    def test_calculate_suites(self):
        evg = Mock()
        evg.test_stats_by_project.return_value = [
            Mock(test_file="test{}.js".format(i), avg_duration_pass=60, num_pass=1)
            for i in range(100)
        ]

        main = grt.Main(evg)
        main.options = Mock()
        main.options.max_sub_suites = 1000
        main.config_options = self.get_mock_options()

        with patch("os.path.exists") as exists_mock, patch(ns("suitesconfig")) as suitesconfig_mock:
            exists_mock.return_value = True
            suitesconfig_mock.get_suite.return_value.tests = \
                [stat.test_file for stat in evg.test_stats_by_project.return_value]
            suites = main.calculate_suites(_DATE, _DATE)

            # There are 100 tests taking 1 minute, with a target of 10 min we expect 10 suites.
            self.assertEqual(10, len(suites))
            for suite in suites:
                self.assertEqual(10, len(suite.tests))

    def test_calculate_suites_fallback(self):
        n_tests = 100
        response = Mock()
        response.status_code = requests.codes.SERVICE_UNAVAILABLE
        evg = Mock()
        evg.test_stats_by_project.side_effect = requests.HTTPError(response=response)

        main = grt.Main(evg)
        main.options = Mock()
        main.options.execution_time_minutes = 10
        main.config_options = self.get_mock_options()
        main.list_tests = Mock(return_value=["test{}.js".format(i) for i in range(n_tests)])

        suites = main.calculate_suites(_DATE, _DATE)

        self.assertEqual(main.config_options.fallback_num_sub_suites, len(suites))
        for suite in suites:
            self.assertEqual(50, len(suite.tests))

        self.assertEqual(n_tests, len(main.test_list))

    def test_calculate_suites_uses_fallback_for_no_results(self):
        n_tests = 100
        evg = Mock()
        evg.test_stats_by_project.return_value = []

        main = grt.Main(evg)
        main.options = Mock()
        main.config_options = self.get_mock_options()
        main.list_tests = Mock(return_value=["test{}.js".format(i) for i in range(n_tests)])
        suites = main.calculate_suites(_DATE, _DATE)

        self.assertEqual(main.config_options.fallback_num_sub_suites, len(suites))
        for suite in suites:
            self.assertEqual(50, len(suite.tests))

        self.assertEqual(n_tests, len(main.test_list))

    def test_calculate_suites_uses_fallback_if_only_results_are_filtered(self):
        n_tests = 100
        evg = Mock()
        evg.test_stats_by_project.return_value = [
            Mock(test_file="test{}.js".format(i), avg_duration_pass=60, num_pass=1)
            for i in range(100)
        ]
        main = grt.Main(evg)
        main.options = Mock()
        main.config_options = self.get_mock_options()
        main.list_tests = Mock(return_value=["test{}.js".format(i) for i in range(n_tests)])
        with patch("os.path.exists") as exists_mock:
            exists_mock.return_value = False
            suites = main.calculate_suites(_DATE, _DATE)

            self.assertEqual(main.config_options.fallback_num_sub_suites, len(suites))
            for suite in suites:
                self.assertEqual(50, len(suite.tests))

            self.assertEqual(n_tests, len(main.test_list))

    def test_calculate_suites_error(self):
        response = Mock()
        response.status_code = requests.codes.INTERNAL_SERVER_ERROR
        evg = Mock()
        evg.test_stats_by_project.side_effect = requests.HTTPError(response=response)

        main = grt.Main(evg)
        main.options = Mock()
        main.options.execution_time_minutes = 10
        main.config_options = self.get_mock_options()
        main.list_tests = Mock(return_value=["test{}.js".format(i) for i in range(100)])

        with self.assertRaises(requests.HTTPError):
            main.calculate_suites(_DATE, _DATE)

    def test_filter_missing_files(self):
        tests_runtimes = [
            ("dir1/file1.js", 20.32),
            ("dir2/file2.js", 24.32),
            ("dir1/file3.js", 36.32),
        ]

        with patch("os.path.exists") as exists_mock, patch(ns("suitesconfig")) as suitesconfig_mock:
            exists_mock.side_effect = [False, True, True]
            evg = Mock()
            suitesconfig_mock.get_suite.return_value.tests = \
                [runtime[0] for runtime in tests_runtimes]
            main = grt.Main(evg)
            main.config_options = Mock()
            main.config_options.suite = "suite"
            filtered_list = main.filter_existing_tests(tests_runtimes)

            self.assertEqual(2, len(filtered_list))
            self.assertNotIn(tests_runtimes[0], filtered_list)
            self.assertIn(tests_runtimes[2], filtered_list)
            self.assertIn(tests_runtimes[1], filtered_list)

    def test_filter_blacklist_files(self):
        tests_runtimes = [
            ("dir1/file1.js", 20.32),
            ("dir2/file2.js", 24.32),
            ("dir1/file3.js", 36.32),
        ]

        blacklisted_test = tests_runtimes[1][0]

        with patch("os.path.exists") as exists_mock, patch(ns("suitesconfig")) as suitesconfig_mock:
            exists_mock.return_value = True
            evg = Mock()
            suitesconfig_mock.get_suite.return_value.tests = \
                [runtime[0] for runtime in tests_runtimes if runtime[0] != blacklisted_test]
            main = grt.Main(evg)
            main.config_options = Mock()
            main.config_options.suite = "suite"
            filtered_list = main.filter_existing_tests(tests_runtimes)

            self.assertEqual(2, len(filtered_list))
            self.assertNotIn(blacklisted_test, filtered_list)
            self.assertIn(tests_runtimes[2], filtered_list)
            self.assertIn(tests_runtimes[0], filtered_list)

    def test_filter_blacklist_files_for_windows(self):
        tests_runtimes = [
            ("dir1/file1.js", 20.32),
            ("dir2/file2.js", 24.32),
            ("dir1/dir3/file3.js", 36.32),
        ]

        blacklisted_test = tests_runtimes[1][0]

        with patch("os.path.exists") as exists_mock, patch(ns("suitesconfig")) as suitesconfig_mock:
            exists_mock.return_value = True
            evg = Mock()
            suitesconfig_mock.get_suite.return_value.tests = [
                runtime[0].replace("/", "\\") for runtime in tests_runtimes
                if runtime[0] != blacklisted_test
            ]
            main = grt.Main(evg)
            main.config_options = Mock()
            main.config_options.suite = "suite"
            filtered_list = main.filter_existing_tests(tests_runtimes)

            self.assertNotIn(blacklisted_test, filtered_list)
            self.assertIn(tests_runtimes[2], filtered_list)
            self.assertIn(tests_runtimes[0], filtered_list)
            self.assertEqual(2, len(filtered_list))


class TestShouldTasksBeGenerated(unittest.TestCase):
    def test_during_first_execution(self):
        evg_api = MagicMock()
        task_id = "task_id"
        evg_api.task_by_id.return_value.execution = 0

        self.assertTrue(grt.should_tasks_be_generated(evg_api, task_id))
        evg_api.task_by_id.assert_called_with(task_id, fetch_all_executions=True)

    def test_after_successful_execution(self):
        evg_api = MagicMock()
        task_id = "task_id"
        task = evg_api.task_by_id.return_value
        task.execution = 1
        task.get_execution.return_value.is_success.return_value = True

        self.assertFalse(grt.should_tasks_be_generated(evg_api, task_id))
        evg_api.task_by_id.assert_called_with(task_id, fetch_all_executions=True)

    def test_after_multiple_successful_execution(self):
        evg_api = MagicMock()
        task_id = "task_id"
        task = evg_api.task_by_id.return_value
        task.execution = 5
        task.get_execution.return_value.is_success.return_value = True

        self.assertFalse(grt.should_tasks_be_generated(evg_api, task_id))
        evg_api.task_by_id.assert_called_with(task_id, fetch_all_executions=True)

    def test_after_failed_execution(self):
        evg_api = MagicMock()
        task_id = "task_id"
        task = evg_api.task_by_id.return_value
        task.execution = 1
        task.get_execution.return_value.is_success.return_value = False

        self.assertTrue(grt.should_tasks_be_generated(evg_api, task_id))
        evg_api.task_by_id.assert_called_with(task_id, fetch_all_executions=True)

    def test_after_multiple_failed_execution(self):
        evg_api = MagicMock()
        task_id = "task_id"
        task = evg_api.task_by_id.return_value
        task.execution = 5
        task.get_execution.return_value.is_success.return_value = False

        self.assertTrue(grt.should_tasks_be_generated(evg_api, task_id))
        evg_api.task_by_id.assert_called_with(task_id, fetch_all_executions=True)
