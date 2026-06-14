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

# hook_timing_stress_log_conn_close.py
#     python run.py --hook hook_timing_stress_log_conn_close
#
# This hook is used to add timing stress "conn_close_stress_log_printf" to all python tests

from __future__ import print_function

import wthooks

# Called to manipulate args for wiredtiger_open
def wiredtiger_open_args(ignored_self, args):
    args = list(args)    # convert from a readonly tuple to a writeable list
    if (len(args) == 1):
        return args
    if "timing_stress_for_test" in args[1]:
        return args

    args[-1] += ',,,timing_stress_for_test=[conn_close_stress_log_printf],,,'   # modify the last arg
    return args

# Every hook file must have one or more classes descended from WiredTigerHook
# This is where the hook functions are 'hooked' to API methods.
class TimingStressLogCreator(wthooks.WiredTigerHookCreator):
    def __init__(self, args=0):
        self.platform_api = wthooks.DefaultPlatformAPI()

    def get_platform_api(self):
        return self.platform_api

    # Skip tests that won't work on non-standalone build
    def register_skipped_test(self, tests):
        # There are no general categories of tests to skip for non-standalone.
        # Individual tests are skipped via the skip_for_hook decorator
        pass

    def setup_hooks(self):
        self.wiredtiger['wiredtiger_open'] = (wthooks.HOOK_ARGS, wiredtiger_open_args)
        pass

# Every hook file must have a top level initialize function,
# returning a list of WiredTigerHook objects.
def initialize(arg):
    return [TimingStressLogCreator(arg)]
