"""Tests for testtools itself."""

# See README for copyright and licensing details.

import unittest


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
        test_testresult,
        test_testsuite,
        test_testtools,
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
        test_testresult,
        test_testsuite,
        test_testtools,
        ]
    suites = map(lambda x: x.test_suite(), modules)
    return unittest.TestSuite(suites)
