"""Unit tests for the generate_resmoke_suite script."""

from __future__ import absolute_import

import datetime
import math
import unittest
import yaml

from mock import patch, mock_open, call

from buildscripts import generate_resmoke_suites as grs
from generate_resmoke_suites import render_suite, render_misc_suite, \
    prepare_directory_for_suite

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use


class GetStartAndEndCommitSinceDateTest(unittest.TestCase):
    @patch('buildscripts.client.github.GithubApi')
    def test_that_first_and_last_commits_returned(self, GithubApi):
        GithubApi.get_commits.return_value = [
            {"sha": "first"},
            {"sha": "second"},
            {"sha": "third"},
        ]

        target = grs.ProjectTarget("owner", "project", "branch")

        today = datetime.datetime.utcnow().replace(microsecond=0, tzinfo=None)

        commitRange = grs.get_start_and_end_commit_since_date(GithubApi, target, today)

        self.assertEqual(commitRange.start, "third")
        self.assertEqual(commitRange.end, "first")


class GetHistoryByRevisionTest(unittest.TestCase):
    @patch('buildscripts.client.evergreen.EvergreenApi')
    def test_get_history_by_revision_call_evergreen(self, EvergreenApi):
        grs.get_history_by_revision(EvergreenApi, '', grs.CommitRange('start', 'end'), '', None)

        self.assertTrue(EvergreenApi.get_history.called)


class GetTestHistoryTest(unittest.TestCase):
    @patch('buildscripts.client.evergreen.EvergreenApi')
    def test_get_test_history_returns_when_the_end_revision_is_given(self, EvergreenApi):
        def get_history_mock(project, params):
            return [{"revision": "end"}]

        EvergreenApi.get_history = get_history_mock
        grs.get_test_history(EvergreenApi, grs.ProjectTarget('', '', ''), '',
                             grs.CommitRange('start', 'end'), None)

    @patch('buildscripts.client.evergreen.EvergreenApi')
    def test_get_test_history_can_be_called_multiple_times(self, EvergreenApi):
        call_data = {"count": 0}

        def get_history_mock(project, params):
            returnValue = [
                "nottheend",
                "end",
            ]

            call_data["count"] += 1
            return [{"revision": returnValue[call_data["count"] - 1]}]

        EvergreenApi.get_history = get_history_mock

        grs.get_test_history(EvergreenApi, grs.ProjectTarget('', '', ''), '',
                             grs.CommitRange('start', 'end'), None)

        self.assertEqual(call_data["count"], 2)


class SplitHookRunsOutTest(unittest.TestCase):
    def test_a_list_with_no_hooks_returns_no_hooks(self):
        test_list = [
            {"test_file": "file1.js"},
            {"test_file": "file2.js"},
            {"test_file": "file3.js"},
            {"test_file": "file4.js"},
            {"test_file": "file5.js"},
        ]

        (tests, hooks) = grs.split_hook_runs_out(test_list)

        self.assertEqual(len(tests), 5)
        self.assertEqual(len(hooks), 0)

    def test_a_list_with_only_hooks_returns_all_hooks(self):
        test_list = [
            {"test_file": "file1:js"},
            {"test_file": "file2:js"},
            {"test_file": "file3:js"},
            {"test_file": "file4:js"},
            {"test_file": "file5:js"},
        ]

        (tests, hooks) = grs.split_hook_runs_out(test_list)

        self.assertEqual(len(tests), 0)
        self.assertEqual(len(hooks), 5)

    def test_a_list_with_a_mix_of_test_and_hooks_returns_both(self):
        test_list = [
            {"test_file": "file1:js"},
            {"test_file": "file2.js"},
            {"test_file": "file3:js"},
            {"test_file": "file4.js"},
            {"test_file": "file5:js"},
        ]

        (tests, hooks) = grs.split_hook_runs_out(test_list)

        self.assertEqual(len(tests), 2)
        self.assertEqual(len(hooks), 3)


class OrganizeHooksTest(unittest.TestCase):
    def test_calling_with_no_executions(self):
        hooks = grs.organize_hooks([])

        self.assertEqual(len(hooks), 0)

    def test_one_hooks(self):
        executions = [{
            "revision": "revision1",
            "variant": "variant1",
            "test_file": "test1:hookName",
            "duration": 1000000000,
        }]

        hooks = grs.organize_hooks(executions)

        self.assertEqual(len(hooks), 1)
        self.assertEqual(hooks["revision1"]["variant1"]["test1"], 1000000000)

    def test_multiple_hooks_on_the_same_test(self):
        executions = [{
            "revision": "revision1",
            "variant": "variant1",
            "test_file": "test1:hookName",
            "duration": 1000000000,
        }, {
            "revision": "revision1",
            "variant": "variant1",
            "test_file": "test1:hookName1",
            "duration": 1000000000,
        }, {
            "revision": "revision1",
            "variant": "variant1",
            "test_file": "test1:hookName2",
            "duration": 1000000000,
        }]

        hooks = grs.organize_hooks(executions)

        self.assertEqual(len(hooks), 1)
        self.assertEqual(hooks["revision1"]["variant1"]["test1"], 3000000000)

    def test_multiple_hooks_on_different_variants(self):
        executions = [{
            "revision": "revision1",
            "variant": "variant1",
            "test_file": "test1:hookName",
            "duration": 1000000000,
        }, {
            "revision": "revision1",
            "variant": "variant2",
            "test_file": "test1:hookName1",
            "duration": 1000000000,
        }, {
            "revision": "revision1",
            "variant": "variant3",
            "test_file": "test1:hookName2",
            "duration": 1000000000,
        }]

        hooks = grs.organize_hooks(executions)

        self.assertEqual(len(hooks), 1)
        self.assertEqual(hooks["revision1"]["variant1"]["test1"], 1000000000)
        self.assertEqual(hooks["revision1"]["variant2"]["test1"], 1000000000)
        self.assertEqual(hooks["revision1"]["variant3"]["test1"], 1000000000)

    def test_multiple_hooks_on_different_revisions(self):
        executions = [{
            "revision": "revision1",
            "variant": "variant1",
            "test_file": "test1:hookName",
            "duration": 1000000000,
        }, {
            "revision": "revision2",
            "variant": "variant1",
            "test_file": "test1:hookName1",
            "duration": 1000000000,
        }, {
            "revision": "revision3",
            "variant": "variant1",
            "test_file": "test1:hookName2",
            "duration": 1000000000,
        }]

        hooks = grs.organize_hooks(executions)

        self.assertEqual(len(hooks), 3)
        self.assertEqual(hooks["revision1"]["variant1"]["test1"], 1000000000)
        self.assertEqual(hooks["revision2"]["variant1"]["test1"], 1000000000)
        self.assertEqual(hooks["revision3"]["variant1"]["test1"], 1000000000)


class ExecutionRuntimeTest(unittest.TestCase):
    def test_execution_runtime_is_calculated_with_no_hooks(self):
        execution = {
            "revision": "revision1",
            "variant": "variant1",
            "duration": 1000000000,
        }

        runtime = grs.execution_runtime("test.js", execution, {})

        self.assertEquals(runtime, 1)

    def test_execution_runtime_is_calculated_with_no_applicable_hooks(self):
        execution = {
            "revision": "revision1",
            "variant": "variant1",
            "duration": 1000000000,
        }
        hooks = {"revision1": {"variant2": {"test": 1000000000, }}}

        runtime = grs.execution_runtime("test.js", execution, hooks)

        self.assertEquals(runtime, 1)

    def test_execution_runtime_is_calculated_with_hooks(self):
        execution = {
            "revision": "revision1",
            "variant": "variant1",
            "duration": 1000000000,
        }
        hooks = {"revision1": {"variant1": {"test": 1000000000, }}}

        runtime = grs.execution_runtime("test.js", execution, hooks)

        self.assertEquals(runtime, 2)


class OrganizeExecutionsByTestTest(unittest.TestCase):
    def test_no_executions(self):
        tests = grs.organize_executions_by_test([])

        self.assertEquals(len(tests), 0)

    @patch("buildscripts.generate_resmoke_suites.os")
    def test_only_test_executions(self, os):
        os.path.isfile.return_value = True

        executions = [{
            "revision": "revision1",
            "variant": "variant1",
            "test_file": "test1.js",
            "duration": 1000000000,
        }, {
            "revision": "revision1",
            "variant": "variant1",
            "test_file": "test2.js",
            "duration": 2000000000,
        }, {
            "revision": "revision1",
            "variant": "variant1",
            "test_file": "test3.js",
            "duration": 3000000000,
        }]

        tests = grs.organize_executions_by_test(executions)

        self.assertEquals(len(tests), 3)
        self.assertEquals(tests["test1.js"]["variant1"], 1)
        self.assertEquals(tests["test2.js"]["variant1"], 2)
        self.assertEquals(tests["test3.js"]["variant1"], 3)

    @patch("buildscripts.generate_resmoke_suites.os")
    def test_mix_of_test_and_hook_executions(self, os):
        os.path.isfile.return_value = True

        executions = [
            {
                "revision": "revision1",
                "variant": "variant1",
                "test_file": "test1.js",
                "duration": 1000000000,
            },
            {
                "revision": "revision1",
                "variant": "variant1",
                "test_file": "test1:js",
                "duration": 1000000000,
            },
            {
                "revision": "revision1",
                "variant": "variant1",
                "test_file": "test2.js",
                "duration": 2000000000,
            },
            {
                "revision": "revision1",
                "variant": "variant1",
                "test_file": "test3:js",
                "duration": 3000000000,
            },
            {
                "revision": "revision1",
                "variant": "variant1",
                "test_file": "test3.js",
                "duration": 3000000000,
            },
        ]

        tests = grs.organize_executions_by_test(executions)

        self.assertEquals(len(tests), 3)
        self.assertEquals(tests["test1.js"]["variant1"], 2)
        self.assertEquals(tests["test2.js"]["variant1"], 2)
        self.assertEquals(tests["test3.js"]["variant1"], 6)

    @patch("buildscripts.generate_resmoke_suites.os")
    def test_multiple_revisions_for_same_test(self, os):
        os.path.isfile.return_value = True

        executions = [
            {
                "revision": "revision1",
                "variant": "variant1",
                "test_file": "test1.js",
                "duration": 1000000000,
            },
            {
                "revision": "revision2",
                "variant": "variant1",
                "test_file": "test1.js",
                "duration": 1000000000,
            },
            {
                "revision": "revision1",
                "variant": "variant1",
                "test_file": "test1:js",
                "duration": 1000000000,
            },
            {
                "revision": "revision1",
                "variant": "variant1",
                "test_file": "test2.js",
                "duration": 2000000000,
            },
            {
                "revision": "revision1",
                "variant": "variant1",
                "test_file": "test3:js",
                "duration": 3000000000,
            },
            {
                "revision": "revision2",
                "variant": "variant1",
                "test_file": "test3.js",
                "duration": 3000000000,
            },
        ]

        tests = grs.organize_executions_by_test(executions)

        self.assertEquals(len(tests), 3)
        self.assertEquals(tests["test1.js"]["variant1"], 1)
        self.assertEquals(tests["test2.js"]["variant1"], 2)
        self.assertEquals(tests["test3.js"]["variant1"], 3)

    @patch("buildscripts.generate_resmoke_suites.os")
    def test_non_files_are_not_included(self, os):
        os.path.isfile.return_value = False

        executions = [{
            "revision": "revision1",
            "variant": "variant1",
            "test_file": "test1.js",
            "duration": 1000000000,
        }, {
            "revision": "revision1",
            "variant": "variant1",
            "test_file": "test2.js",
            "duration": 2000000000,
        }, {
            "revision": "revision1",
            "variant": "variant1",
            "test_file": "test3.js",
            "duration": 3000000000,
        }]

        tests = grs.organize_executions_by_test(executions)

        self.assertEquals(len(tests), 0)


class DivideRemainingTestsAmongSuitesTest(unittest.TestCase):
    @staticmethod
    def generate_tests(n_tests):
        tests = {}
        test_names = []
        for idx in range(n_tests):
            name = "test_{0}".format(idx)
            test_names.append(name)
            tests[name] = {"max_runtime": 2 * idx}

        return test_names, tests

    def test_each_suite_gets_one_test(self):
        suites = [grs.Suite(), grs.Suite(), grs.Suite()]
        test_names, tests = self.generate_tests(3)

        grs.divide_remaining_tests_among_suites(test_names, tests, suites)

        for suite in suites:
            self.assertEqual(suite.get_test_count(), 1)

    def test_each_suite_gets_at_least_one_test(self):
        suites = [grs.Suite(), grs.Suite(), grs.Suite()]
        test_names, tests = self.generate_tests(5)

        grs.divide_remaining_tests_among_suites(test_names, tests, suites)

        total_tests = 0
        for suite in suites:
            total_tests += suite.get_test_count()
            self.assertGreaterEqual(suite.get_test_count(), 1)

        self.assertEqual(total_tests, len(tests))


class DivideTestsIntoSuitesByMaxtimeTest(unittest.TestCase):
    def test_if_less_total_than_max_only_one_suite_created(self):
        max_time = 20
        test_names = ["test1", "test2", "test3"]
        tests = {
            test_names[0]: {"max_runtime": 5},
            test_names[1]: {"max_runtime": 4},
            test_names[2]: {"max_runtime": 3},
        }

        suites = grs.divide_tests_into_suites_by_maxtime(tests, test_names, max_time)
        self.assertEqual(len(suites), 1)
        self.assertEqual(suites[0].get_test_count(), 3)
        self.assertEqual(suites[0].get_runtime(), 12)

    def test_if_each_test_should_be_own_suite(self):
        max_time = 5
        test_names = ["test1", "test2", "test3"]
        tests = {
            test_names[0]: {"max_runtime": 5},
            test_names[1]: {"max_runtime": 4},
            test_names[2]: {"max_runtime": 3},
        }

        suites = grs.divide_tests_into_suites_by_maxtime(tests, test_names, max_time)
        self.assertEqual(len(suites), 3)

    def test_if_test_is_greater_than_max_it_goes_alone(self):
        max_time = 7
        test_names = ["test1", "test2", "test3"]
        tests = {
            test_names[0]: {"max_runtime": 15},
            test_names[1]: {"max_runtime": 4},
            test_names[2]: {"max_runtime": 3},
        }

        suites = grs.divide_tests_into_suites_by_maxtime(tests, test_names, max_time)
        self.assertEqual(len(suites), 2)
        self.assertEqual(suites[0].get_test_count(), 1)
        self.assertEqual(suites[0].get_runtime(), 15)

    def test_max_sub_suites_options(self):
        max_time = 5
        max_suites = 2
        test_names = ["test1", "test2", "test3", "test4", "test5"]
        tests = {
            test_names[0]: {"max_runtime": 5},
            test_names[1]: {"max_runtime": 4},
            test_names[2]: {"max_runtime": 3},
            test_names[3]: {"max_runtime": 4},
            test_names[4]: {"max_runtime": 3},
        }

        suites = grs.divide_tests_into_suites_by_maxtime(tests, test_names, max_time,
                                                         max_suites=max_suites)
        self.assertEqual(len(suites), max_suites)
        total_tests = 0
        for suite in suites:
            total_tests += suite.get_test_count()
        self.assertEqual(total_tests, len(test_names))


class SuiteTest(unittest.TestCase):
    def test_adding_tests_increases_count_and_runtime(self):
        suite = grs.Suite()
        suite.add_test('test1', {
            "max_runtime": 10,
            "variant1": 5,
            "variant2": 10,
            "variant3": 7,
        })
        suite.add_test('test2', {
            "max_runtime": 12,
            "variant1": 12,
            "variant2": 8,
            "variant3": 6,
        })
        suite.add_test('test3', {
            "max_runtime": 7,
            "variant1": 6,
            "variant2": 6,
            "variant3": 7,
        })

        self.assertEqual(suite.get_test_count(), 3)
        self.assertEqual(suite.get_runtime(), 29)


def create_suite(count=3, start=0):
    """ Create a suite with count tests."""
    suite = grs.Suite()
    for i in range(start, start + count):
        suite.add_test('test{}'.format(i), {})
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
        with patch('generate_resmoke_suites.open', m, create=True):
            render_suite(suites, 'suite_name', 'tmp')
        handle = m()

        # The other writes are for the headers.
        self.assertEquals(len(suites) * 2, handle.write.call_count)
        handle.write.assert_has_calls([call(e) for e in expected], any_order=True)
        calls = [
            call('buildscripts/resmokeconfig/suites/suite_name.yml', 'r')
            for _ in range(len(suites))
        ]
        m.assert_has_calls(calls, any_order=True)
        filename = 'tmp/suite_name_{{:0{}}}.yml'.format(int(math.ceil(math.log10(size))))
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
        with patch('generate_resmoke_suites.open', m, create=True):
            render_misc_suite(test_list, 'suite_name', 'tmp')
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
        calls = [call('buildscripts/resmokeconfig/suites/suite_name.yml', 'r')]
        m.assert_has_calls(calls, any_order=True)
        filename = 'tmp/suite_name_misc.yml'
        calls = [call(filename, 'w')]
        m.assert_has_calls(calls, any_order=True)


class PrepareDirectoryForSuite(unittest.TestCase):
    def test_no_directory(self):
        with patch('generate_resmoke_suites.os') as mock_os,\
             patch('generate_resmoke_suites.glob.glob') as mock_glob:
            mock_os.path.exists.return_value = False
            prepare_directory_for_suite('suite_name', 'tmp')

        mock_os.makedirs.assert_called_once_with('tmp')
        mock_glob.assert_not_called()

    def _test(self, matched=None):
        if matched is None:
            matched = []
        with patch('generate_resmoke_suites.os') as mock_os, \
                patch('generate_resmoke_suites.glob.glob') as mock_glob:
            mock_os.path.exists.return_value = True
            mock_glob.side_effect = (matched, [])
            prepare_directory_for_suite('suite_name', 'tmp')

        mock_glob.assert_has_calls(
            [call('tmp/suite_name_[0-9]*.yml'),
             call('tmp/suite_name_misc.yml')])
        if matched:
            mock_os.remove.assert_has_calls([call(filename) for filename in matched])
        else:
            mock_os.remove.assert_not_called()
        mock_os.makedirs.assert_not_called()

    def test_empty_directory(self):
        self._test()

    def test_old_suite_files(self):
        self._test(matched=['tmp/suite_name_{}.yml'.format(i) for i in range(3)])
