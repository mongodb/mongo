# Copyright (c) 2010-2012 testtools developers. See LICENSE for details.

from testtools import TestCase
from testtools.tests.helpers import (
    FullStackRunTest,
    hide_testtools_stack,
    is_stack_hidden,
    )


class TestStackHiding(TestCase):

    run_tests_with = FullStackRunTest

    def setUp(self):
        super(TestStackHiding, self).setUp()
        self.addCleanup(hide_testtools_stack, is_stack_hidden())

    def test_is_stack_hidden_consistent_true(self):
        hide_testtools_stack(True)
        self.assertEqual(True, is_stack_hidden())

    def test_is_stack_hidden_consistent_false(self):
        hide_testtools_stack(False)
        self.assertEqual(False, is_stack_hidden())


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
