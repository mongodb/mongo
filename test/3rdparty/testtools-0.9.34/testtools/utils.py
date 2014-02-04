# Copyright (c) 2008-2010 testtools developers. See LICENSE for details.

"""Utilities for dealing with stuff in unittest.

Legacy - deprecated - use testtools.testsuite.iterate_tests
"""

import warnings
warnings.warn("Please import iterate_tests from testtools.testsuite - "
    "testtools.utils is deprecated.", DeprecationWarning, stacklevel=2)

from testtools.testsuite import iterate_tests

