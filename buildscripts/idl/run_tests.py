#!/usr/bin/env python2
# Copyright (C) 2017 MongoDB Inc.
#
# This program is free software: you can redistribute it and/or  modify
# it under the terms of the GNU Affero General Public License, version 3,
# as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
"""
IDL Unit Test runner

Generates a file called results.xml in the XUnit format.
"""

import sys
import unittest
from xmlrunner import XMLTestRunner


def run_tests():
    # type: () -> None
    """Run all the tests."""

    # my-py's typeshed does not have defaultTestLoader and TestLoader type information so suppresss
    # my-py type information.
    all_tests = unittest.defaultTestLoader.discover(start_dir="tests")  # type: ignore

    with open("results.xml", "wb") as output:

        runner = XMLTestRunner(verbosity=2, failfast=False, output=output)
        result = runner.run(all_tests)

    sys.exit(not result.wasSuccessful())


if __name__ == '__main__':
    run_tests()
