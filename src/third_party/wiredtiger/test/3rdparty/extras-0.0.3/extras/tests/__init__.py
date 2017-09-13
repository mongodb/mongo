# Copyright (c) 2010-2012 extras developers. See LICENSE for details.

"""Tests for extras."""

from unittest import TestSuite, TestLoader


def test_suite():
    from extras.tests import (
        test_extras,
        )
    modules = [
        test_extras,
        ]
    loader = TestLoader()
    suites = map(loader.loadTestsFromModule, modules)
    return TestSuite(suites)
