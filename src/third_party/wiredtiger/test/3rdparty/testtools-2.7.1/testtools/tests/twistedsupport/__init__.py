# Copyright (c) testtools developers. See LICENSE for details.

from unittest import TestSuite


def test_suite():
    from testtools.tests.twistedsupport import (
        test_deferred,
        test_matchers,
        test_runtest,
        test_spinner,
    )
    modules = [
        test_deferred,
        test_matchers,
        test_runtest,
        test_spinner,
    ]
    suites = map(lambda x: x.test_suite(), modules)
    return TestSuite(suites)
