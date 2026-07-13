#!/usr/bin/env python3
# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
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
