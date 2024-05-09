# Copyright (c) 2009-2012 testtools developers. See LICENSE for details.


from unittest import TestSuite


def test_suite():
    from testtools.tests.matchers import (
        test_basic,
        test_const,
        test_datastructures,
        test_dict,
        test_doctest,
        test_exception,
        test_filesystem,
        test_higherorder,
        test_impl,
        test_warnings
        )
    modules = [
        test_basic,
        test_const,
        test_datastructures,
        test_dict,
        test_doctest,
        test_exception,
        test_filesystem,
        test_higherorder,
        test_impl,
        test_warnings
        ]
    suites = map(lambda x: x.test_suite(), modules)
    return TestSuite(suites)
