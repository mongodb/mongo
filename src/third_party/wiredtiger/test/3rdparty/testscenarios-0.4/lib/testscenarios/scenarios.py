#  testscenarios: extensions to python unittest to allow declarative
#  dependency injection ('scenarios') by tests.
#
# Copyright (c) 2009, Robert Collins <robertc@robertcollins.net>
# Copyright (c) 2010, 2011 Martin Pool <mbp@sourcefrog.net>
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

__all__ = [
    'apply_scenario',
    'apply_scenarios',
    'generate_scenarios',
    'load_tests_apply_scenarios',
    'multiply_scenarios',
    ]

from itertools import (
    chain,
    product,
    )
import sys
import unittest

from testtools.testcase import clone_test_with_new_id
from testtools import iterate_tests


def apply_scenario(scenario, test):
    """Apply scenario to test.

    :param scenario: A tuple (name, parameters) to apply to the test. The test
        is cloned, its id adjusted to have (name) after it, and the parameters
        dict is used to update the new test.
    :param test: The test to apply the scenario to. This test is unaltered.
    :return: A new test cloned from test, with the scenario applied.
    """
    name, parameters = scenario
    scenario_suffix = '(' + name + ')'
    newtest = clone_test_with_new_id(test,
        test.id() + scenario_suffix)
    test_desc = test.shortDescription()
    if test_desc is not None:
        newtest_desc = "%(test_desc)s %(scenario_suffix)s" % vars()
        newtest.shortDescription = (lambda: newtest_desc)
    for key, value in parameters.items():
        setattr(newtest, key, value)
    return newtest


def apply_scenarios(scenarios, test):
    """Apply many scenarios to a test.

    :param scenarios: An iterable of scenarios.
    :param test: A test to apply the scenarios to.
    :return: A generator of tests.
    """
    for scenario in scenarios:
        yield apply_scenario(scenario, test)


def generate_scenarios(test_or_suite):
    """Yield the tests in test_or_suite with scenario multiplication done.

    TestCase objects with no scenarios specified are yielded unaltered. Tests
    with scenarios are not yielded at all, instead the results of multiplying
    them by the scenarios they specified gets yielded.

    :param test_or_suite: A TestCase or TestSuite.
    :return: A generator of tests - objects satisfying the TestCase protocol.
    """
    for test in iterate_tests(test_or_suite):
        scenarios = getattr(test, 'scenarios', None)
        if scenarios:
            for newtest in apply_scenarios(scenarios, test):
                newtest.scenarios = None
                yield newtest
        else:
            yield test


def load_tests_apply_scenarios(*params):
    """Adapter test runner load hooks to call generate_scenarios.

    If this is referenced by the `load_tests` attribute of a module, then
    testloaders that implement this protocol will automatically arrange for
    the scenarios to be expanded. This can be used instead of using
    TestWithScenarios.

    Two different calling conventions for load_tests have been used, and this
    function should support both. Python 2.7 passes (loader, standard_tests,
    pattern), and bzr used (standard_tests, module, loader).

    :param loader: A TestLoader.
    :param standard_test: The test objects found in this module before 
        multiplication.
    """
    if getattr(params[0], 'suiteClass', None) is not None:
        loader, standard_tests, pattern = params
    else:
        standard_tests, module, loader = params
    result = loader.suiteClass()
    result.addTests(generate_scenarios(standard_tests))
    return result


def multiply_scenarios(*scenarios):
    """Multiply two or more iterables of scenarios.

    It is safe to pass scenario generators or iterators.

    :returns: A list of compound scenarios: the cross-product of all 
        scenarios, with the names concatenated and the parameters
        merged together.
    """
    result = []
    scenario_lists = map(list, scenarios)
    for combination in product(*scenario_lists):
        names, parameters = zip(*combination)
        scenario_name = ','.join(names)
        scenario_parameters = {}
        for parameter in parameters:
            scenario_parameters.update(parameter)
        result.append((scenario_name, scenario_parameters))
    return result


def per_module_scenarios(attribute_name, modules):
    """Generate scenarios for available implementation modules.

    This is typically used when there is a subsystem implemented, for
    example, in both Python and C, and we want to apply the same tests to
    both, but the C module may sometimes not be available.

    Note: if the module can't be loaded, the sys.exc_info() tuple for the
    exception raised during import of the module is used instead of the module
    object. A common idiom is to check in setUp for that and raise a skip or
    error for that case. No special helpers are supplied in testscenarios as
    yet.

    :param attribute_name: A name to be set in the scenario parameter
        dictionary (and thence onto the test instance) pointing to the 
        implementation module (or import exception) for this scenario.

    :param modules: An iterable of (short_name, module_name), where 
        the short name is something like 'python' to put in the
        scenario name, and the long name is a fully-qualified Python module
        name.
    """
    scenarios = []
    for short_name, module_name in modules:
        try:
            mod = __import__(module_name, {}, {}, [''])
        except:
            mod = sys.exc_info()
        scenarios.append((
            short_name, 
            {attribute_name: mod}))
    return scenarios
