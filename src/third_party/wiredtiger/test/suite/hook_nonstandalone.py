#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#
# [TEST_TAGS]
# ignored_file
# [END_TAGS]

# hook_non_standalone.py
#   Skip tests that are not compatible to non-standalone build:
#     python run.py --hook nonstandalone base01
#
# This hook is used to test the non-standalone build 

from __future__ import print_function

import unittest, wthooks
from urllib.parse import parse_qsl

# Every hook file must have one or more classes descended from WiredTigerHook
# This is where the hook functions are 'hooked' to API methods.
class NonStandAloneHookCreator(wthooks.WiredTigerHookCreator):
    def __init__(self, args=0):
        self.platform_api = wthooks.DefaultPlatformAPI()

    def get_platform_api(self):
        return self.platform_api

    # Is this test one we should skip?
    def skip_test(self, test):
        # Skip any test that contains one of these strings as a substring
        skip = [
                # Skip all tests that do timestamped truncate operations.
                "test_checkpoint25.test_checkpoint",
                "test_rollback_to_stable34.test_rollback_to_stable",
                "test_rollback_to_stable36.test_rollback_to_stable36",
                "test_truncate09.test_truncate09",
                "test_truncate15.test_truncate15",

                # This group fail within Python for various, sometimes unknown, reasons.
                "test_bug018.test_bug018"
                ]

        for item in skip:
            if item in str(test):
                return True
        return False

    # Remove tests that won't work on non-standalone build
    def filter_tests(self, tests):
        new_tests = unittest.TestSuite()
        new_tests.addTests([t for t in tests if not self.skip_test(t)])
        return new_tests

    def setup_hooks(self):
        pass

# Every hook file must have a top level initialize function,
# returning a list of WiredTigerHook objects.
def initialize(arg):
    return [NonStandAloneHookCreator(arg)]
