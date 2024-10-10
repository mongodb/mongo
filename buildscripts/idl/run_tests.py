#!/usr/bin/env python3
#
# Copyright (C) 2018-present MongoDB, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the Server Side Public License, version 1,
# as published by MongoDB, Inc.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# Server Side Public License for more details.
#
# You should have received a copy of the Server Side Public License
# along with this program. If not, see
# <http://www.mongodb.com/licensing/server-side-public-license>.
#
# As a special exception, the copyright holders give permission to link the
# code of portions of this program with the OpenSSL library under certain
# conditions as described in each individual source file and distribute
# linked combinations including the program with the OpenSSL library. You
# must comply with the Server Side Public License in all respects for
# all of the code used other than as permitted herein. If you modify file(s)
# with this exception, you may extend this exception to your version of the
# file(s), but you are not obligated to do so. If you do not wish to do so,
# delete this exception statement from your version. If you delete this
# exception statement from all source files in the program, then also delete
# it in the license file.
#
"""
IDL Unit Test runner.

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

    runner = XMLTestRunner(verbosity=2, failfast=False, output="results")
    result = runner.run(all_tests)

    sys.exit(not result.wasSuccessful())


if __name__ == "__main__":
    run_tests()
