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


"""Support for running tests with different scenarios declaratively

Testscenarios provides clean dependency injection for python unittest style
tests. This can be used for interface testing (testing many implementations via
a single test suite) or for classic dependency injection (provide tests with
dependencies externally to the test code itself, allowing easy testing in
different situations).

See the README for a manual, and the docstrings on individual functions and
methods for details.
"""

# same format as sys.version_info: "A tuple containing the five components of
# the version number: major, minor, micro, releaselevel, and serial. All
# values except releaselevel are integers; the release level is 'alpha',
# 'beta', 'candidate', or 'final'. The version_info value corresponding to the
# Python version 2.0 is (2, 0, 0, 'final', 0)."  Additionally we use a
# releaselevel of 'dev' for unreleased under-development code.
#
# If the releaselevel is 'alpha' then the major/minor/micro components are not
# established at this point, and setup.py will use a version of next-$(revno).
# If the releaselevel is 'final', then the tarball will be major.minor.micro.
# Otherwise it is major.minor.micro~$(revno).
__version__ = (0, 4, 0, 'final', 0)

__all__ = [
    'TestWithScenarios',
    'WithScenarios',
    'apply_scenario',
    'apply_scenarios',
    'generate_scenarios',
    'load_tests_apply_scenarios',
    'multiply_scenarios',
    'per_module_scenarios',
    ]


import unittest

from testscenarios.scenarios import (
    apply_scenario,
    generate_scenarios,
    load_tests_apply_scenarios,
    multiply_scenarios,
    per_module_scenarios,
    )
from testscenarios.testcase import TestWithScenarios, WithScenarios


def test_suite():
    import testscenarios.tests
    return testscenarios.tests.test_suite()


def load_tests(standard_tests, module, loader):
    standard_tests.addTests(loader.loadTestsFromNames(["testscenarios.tests"]))
    return standard_tests
