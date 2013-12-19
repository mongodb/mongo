# Copyright (c) 2008-2013 testtools developers. See LICENSE for details.

"""Tests for testtools itself."""


from unittest import TestSuite


def test_suite():
    from testtools.tests import (
        matchers,
        test_compat,
        test_content,
        test_content_type,
        test_deferredruntest,
        test_distutilscmd,
        test_fixturesupport,
        test_helpers,
        test_monkey,
        test_run,
        test_runtest,
        test_spinner,
        test_tags,
        test_testcase,
        test_testresult,
        test_testsuite,
        )
    modules = [
        matchers,
        test_compat,
        test_content,
        test_content_type,
        test_deferredruntest,
        test_distutilscmd,
        test_fixturesupport,
        test_helpers,
        test_monkey,
        test_run,
        test_runtest,
        test_spinner,
        test_tags,
        test_testcase,
        test_testresult,
        test_testsuite,
        ]
    suites = map(lambda x: x.test_suite(), modules)
    return TestSuite(suites)
