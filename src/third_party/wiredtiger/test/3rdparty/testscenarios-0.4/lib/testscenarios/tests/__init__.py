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

import doctest
import sys
import unittest

import testscenarios


def test_suite():
    result = unittest.TestSuite()
    standard_tests = unittest.TestSuite()
    module = sys.modules['testscenarios.tests']
    loader = unittest.TestLoader()
    return load_tests(standard_tests, module, loader)


def load_tests(standard_tests, module, loader):
    test_modules = [
        'testcase',
        'scenarios',
        ]
    prefix = "testscenarios.tests.test_"
    test_mod_names = [prefix + test_module for test_module in test_modules]
    standard_tests.addTests(loader.loadTestsFromNames(test_mod_names))
    doctest.set_unittest_reportflags(doctest.REPORT_ONLY_FIRST_FAILURE)
    standard_tests.addTest(
        doctest.DocFileSuite("../../../README", optionflags=doctest.ELLIPSIS))
    return loader.suiteClass(testscenarios.generate_scenarios(standard_tests))
