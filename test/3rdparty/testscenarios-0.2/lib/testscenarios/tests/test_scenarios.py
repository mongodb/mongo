#  testscenarios: extensions to python unittest to allow declarative
#  dependency injection ('scenarios') by tests.
#
# Copyright (c) 2009, Robert Collins <robertc@robertcollins.net>
# 
# Licensed under either the Apache License, Version 2.0 or the BSD 3-clause
# license at the users choice. A copy of both licenses are available in the
# project source as Apache-2.0 and BSD. You may not use this file except in
# compliance with one of these two licences.
# 
# Unless required by applicable law or agreed to in writing, software
# distributed under these licenses is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# license you chose for the specific language governing permissions and
# limitations under that license.

import unittest

import testscenarios
from testscenarios.scenarios import (
    apply_scenario,
    apply_scenarios,
    generate_scenarios,
    )
import testtools
from testtools.tests.helpers import LoggingResult


class TestGenerateScenarios(testtools.TestCase):

    def hook_apply_scenarios(self):
        self.addCleanup(setattr, testscenarios.scenarios, 'apply_scenarios',
            apply_scenarios)
        log = []
        def capture(scenarios, test):
            log.append((scenarios, test))
            return apply_scenarios(scenarios, test)
        testscenarios.scenarios.apply_scenarios = capture
        return log

    def test_generate_scenarios_preserves_normal_test(self):
        class ReferenceTest(unittest.TestCase):
            def test_pass(self):
                pass
        test = ReferenceTest("test_pass")
        log = self.hook_apply_scenarios()
        self.assertEqual([test], list(generate_scenarios(test)))
        self.assertEqual([], log)

    def test_tests_with_scenarios_calls_apply_scenarios(self):
        class ReferenceTest(unittest.TestCase):
            scenarios = [('demo', {})]
            def test_pass(self):
                pass
        test = ReferenceTest("test_pass")
        log = self.hook_apply_scenarios()
        tests = list(generate_scenarios(test))
        self.assertEqual(
            'testscenarios.tests.test_scenarios.ReferenceTest.test_pass(demo)',
            tests[0].id())
        self.assertEqual([([('demo', {})], test)], log)

    def test_all_scenarios_yielded(self):
        class ReferenceTest(unittest.TestCase):
            scenarios = [('1', {}), ('2', {})]
            def test_pass(self):
                pass
        test = ReferenceTest("test_pass")
        tests = list(generate_scenarios(test))
        self.assertEqual(
            'testscenarios.tests.test_scenarios.ReferenceTest.test_pass(1)',
            tests[0].id())
        self.assertEqual(
            'testscenarios.tests.test_scenarios.ReferenceTest.test_pass(2)',
            tests[1].id())

    def test_scenarios_attribute_cleared(self):
        class ReferenceTest(unittest.TestCase):
            scenarios = [
                ('1', {'foo': 1, 'bar': 2}),
                ('2', {'foo': 2, 'bar': 4})]
            def test_check_foo(self):
                pass
        test = ReferenceTest("test_check_foo")
        tests = list(generate_scenarios(test))
        for adapted in tests:
            self.assertEqual(None, adapted.scenarios)

    def test_multiple_tests(self):
        class Reference1(unittest.TestCase):
            scenarios = [('1', {}), ('2', {})]
            def test_something(self):
                pass
        class Reference2(unittest.TestCase):
            scenarios = [('3', {}), ('4', {})]
            def test_something(self):
                pass
        suite = unittest.TestSuite()
        suite.addTest(Reference1("test_something"))
        suite.addTest(Reference2("test_something"))
        tests = list(generate_scenarios(suite))
        self.assertEqual(4, len(tests))


class TestApplyScenario(testtools.TestCase):

    def setUp(self):
        super(TestApplyScenario, self).setUp()

        self.scenario_name = 'demo'
        self.scenario_attrs = {'foo': 'bar'}
        self.scenario = (self.scenario_name, self.scenario_attrs)

        class ReferenceTest(unittest.TestCase):
            def test_pass(self):
                pass
            def test_pass_with_docstring(self):
                """ The test that always passes.

                    This test case has a PEP 257 conformant docstring,
                    with its first line being a brief synopsis and the
                    rest of the docstring explaining that this test
                    does nothing but pass unconditionally.

                    """
                pass

        self.ReferenceTest = ReferenceTest

    def test_sets_specified_id(self):
        raw_test = self.ReferenceTest('test_pass')
        raw_id = "testscenarios.tests.test_scenarios.ReferenceTest.test_pass"
        scenario_name = self.scenario_name
        expect_id = "%(raw_id)s(%(scenario_name)s)" % vars()
        modified_test = apply_scenario(self.scenario, raw_test)
        self.assertEqual(expect_id, modified_test.id())

    def test_sets_specified_attributes(self):
        raw_test = self.ReferenceTest('test_pass')
        modified_test = apply_scenario(self.scenario, raw_test)
        self.assertEqual('bar', modified_test.foo)

    def test_appends_scenario_name_to_short_description(self):
        raw_test = self.ReferenceTest('test_pass_with_docstring')
        modified_test = apply_scenario(self.scenario, raw_test)
        raw_doc = self.ReferenceTest.test_pass_with_docstring.__doc__
        raw_desc = raw_doc.split("\n")[0].strip()
        scenario_name = self.scenario_name
        expect_desc = "%(raw_desc)s (%(scenario_name)s)" % vars()
        self.assertEqual(expect_desc, modified_test.shortDescription())

class TestApplyScenarios(testtools.TestCase):

    def test_calls_apply_scenario(self):
        self.addCleanup(setattr, testscenarios.scenarios, 'apply_scenario',
            apply_scenario)
        log = []
        def capture(scenario, test):
            log.append((scenario, test))
        testscenarios.scenarios.apply_scenario = capture
        scenarios = ["foo", "bar"]
        result = list(apply_scenarios(scenarios, "test"))
        self.assertEqual([('foo', 'test'), ('bar', 'test')], log)

    def test_preserves_scenarios_attribute(self):
        class ReferenceTest(unittest.TestCase):
            scenarios = [('demo', {})]
            def test_pass(self):
                pass
        test = ReferenceTest("test_pass")
        tests = list(apply_scenarios(ReferenceTest.scenarios, test))
        self.assertEqual([('demo', {})], ReferenceTest.scenarios)
        self.assertEqual(ReferenceTest.scenarios, tests[0].scenarios)
