# Copyright (C) 2018 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html

import unittest

from . import filtration_test

def load_tests(loader, tests, pattern):
    suite = unittest.TestSuite()
    suite.addTest(filtration_test.suite)
    return suite

if __name__ == '__main__':
    unittest.main()
