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

__all__ = [
    'TestWithScenarios',
    ]

import unittest

from testtools.testcase import clone_test_with_new_id

from testscenarios.scenarios import generate_scenarios

class TestWithScenarios(unittest.TestCase):
    """A TestCase with support for scenarios via a scenarios attribute.
    
    When a test object which is an instance of TestWithScenarios is run,
    and there is a non-empty scenarios attribute on the object, the test is
    multiplied by the run method into one test per scenario. For this to work
    reliably the TestWithScenarios.run method must not be overriden in a
    subclass (or overridden compatibly with TestWithScenarios).
    """

    def _get_scenarios(self):
        return getattr(self, 'scenarios', None)

    def countTestCases(self):
        scenarios = self._get_scenarios()
        if not scenarios:
            return 1
        else:
            return len(scenarios)

    def debug(self):
        scenarios = self._get_scenarios()
        if scenarios:
            for test in generate_scenarios(self):
                test.debug()
        else:
            return super(TestWithScenarios, self).debug()

    def run(self, result=None):
        scenarios = self._get_scenarios()
        if scenarios:
            for test in generate_scenarios(self):
                test.run(result)
            return
        else:
            return super(TestWithScenarios, self).run(result)
