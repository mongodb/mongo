#  testscenarios: extensions to python unittest to allow declarative
#  dependency injection ('scenarios') by tests.
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

"""Example TestScenario."""

from testscenarios import TestWithScenarios


scenario1 = ('basic', {'attribute': 'value'})
scenario2 = ('advanced', {'attribute': 'value2'})


class SampleWithScenarios(TestWithScenarios):

    scenarios = [scenario1, scenario2]
    
    def test_demo(self):
        self.assertIsInstance(self.attribute, str)
