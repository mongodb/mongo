"""Tests for testtools itself."""

# See README for copyright and licensing details.

from unittest import TestSuite


def test_suite():
    from testtools.tests import (
        test_compat,
        test_content,
        test_content_type,
        test_deferredruntest,
        test_distutilscmd,
        test_fixturesupport,
        test_helpers,
        test_matchers,
        test_monkey,
        test_run,
        test_runtest,
        test_spinner,
        test_testcase,
        test_testresult,
        test_testsuite,
        )
    modules = [
        test_compat,
        test_content,
        test_content_type,
        test_deferredruntest,
        test_distutilscmd,
        test_fixturesupport,
        test_helpers,
        test_matchers,
        test_monkey,
        test_run,
        test_runtest,
        test_spinner,
        test_testcase,
        test_testresult,
        test_testsuite,
        ]
    suites = map(lambda x: x.test_suite(), modules)
    return TestSuite(suites)
