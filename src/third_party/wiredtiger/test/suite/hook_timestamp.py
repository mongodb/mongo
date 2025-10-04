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

# hook_timestamp.py
#
# Insert the use of timestamps into data sets.
#
# These hooks have three functions.  The primary one is setting up the platform API to return
# a "timestamper".  The dataset package uses this platform API, and so will run with timestamps
# when this hook is enabled.  The timestamper provides a timestamping cursor that "knows" to wrap
# timestamped transactions around certain operations, like insert.
#
# Secondly, we set hooks on the transaction APIs so we know when a transaction has been started or
# finished by the test application. If the application already a transaction in progress,
# the timestamping cursor should not try to open a transaction, but can place a timestamp on the
# current transaction.
#
# To run, for example, the cursor tests with these hooks enabled:
#     ../test/suite/run.py --hook timestamp cursor
#
from __future__ import print_function

import sys, unittest, wthooks, wttest, wttimestamp

# These are the hook functions that are run when particular APIs are called.

# Called to replace Session.begin_transaction
def session_begin_transaction_replace(orig_session_begin_transaction, session_self, config):
    ret = orig_session_begin_transaction(session_self, config)
    session_self._has_transaction = True
    return ret

# Called to replace Session.commit_transaction
def session_commit_transaction_replace(orig_session_commit_transaction, session_self, config):
    ret = orig_session_commit_transaction(session_self, config)
    session_self._has_transaction = False
    return ret

# Called to replace Session.rollback_transaction
def session_rollback_transaction_replace(orig_session_rollback_transaction, session_self, config):
    ret = orig_session_rollback_transaction(session_self, config)
    session_self._has_transaction = False
    return ret

def make_dataset_names():
    import wtdataset
    names = ['wtdataset']
    #g = globals(sys.modules['wtdataset'])
    g = sys.modules['wtdataset'].__dict__
    for name in g:
        if name.endswith('DataSet'):
            names.append(name)
    return names

# Every hook file must have one or more classes descended from WiredTigerHook
# This is where the hook functions are 'hooked' to API methods.
class TimestampHookCreator(wthooks.WiredTigerHookCreator):
    def __init__(self, arg=0):
        # Caller can specify an optional command-line argument.  We're not using it
        # now, but this is where it would show up.

        # Override some platform APIs
        self.platform_api = TimestampPlatformAPI()

    # This hook plays with timestamps, indirectly by modifying the behavior of the *DataSet classes.
    # Here we declare our use of timestamp code, so that tests that have their own notion of
    # timestamps can be skipped when running with this hook.
    def uses(self, use_list):
        if "timestamp" in use_list:
            return True
        return False

    dataset_names = make_dataset_names()

    # Determine whether a test should be skipped, if it should also return the reason for skipping.
    # The timestamp hook skips tests that don't use datasets.
    def should_skip(self, test) -> (bool, str):
        testname = str(test)
        modname = testname.split('.')[0]
        g = sys.modules[modname].__dict__

        uses_dataset = False
        for dsname in self.dataset_names:
            if dsname in g:
                uses_dataset = True
                break

        if not uses_dataset:
            return (True, "Doesn't use dataset")
        else:
            return (False, None)

    # Skip tests that won't work on timestamp cursors
    def register_skipped_tests(self, tests):
        for t in tests:
            (should_skip, skip_reason) = self.should_skip(t)
            if should_skip:
                wttest.register_skipped_test(t, "timestamp", skip_reason)

    def get_platform_api(self):
        return self.platform_api

    def setup_hooks(self):
        orig_session_begin_transaction = self.Session['begin_transaction']
        self.Session['begin_transaction'] =  (wthooks.HOOK_REPLACE, lambda s, config=None:
          session_begin_transaction_replace(orig_session_begin_transaction, s, config))

        orig_session_commit_transaction = self.Session['commit_transaction']
        self.Session['commit_transaction'] =  (wthooks.HOOK_REPLACE, lambda s, config=None:
          session_commit_transaction_replace(orig_session_commit_transaction, s, config))

        orig_session_rollback_transaction = self.Session['rollback_transaction']
        self.Session['rollback_transaction'] =  (wthooks.HOOK_REPLACE, lambda s, config=None:
          session_rollback_transaction_replace(orig_session_rollback_transaction, s, config))

# Override some platform APIs for this hook.
class TimestampPlatformAPI(wthooks.WiredTigerHookPlatformAPI):
    def setUp(self, testcase):
        self._timestamp = wttimestamp.WiredTigerTimeStamp()

    def tearDown(self, testcase):
        pass

    # Return a timestamping implementation, it will be used by the data set classes.
    def getTimestamp(self):
        return self._timestamp


# Every hook file must have a top level initialize function,
# returning a list of WiredTigerHook objects.
def initialize(arg):
    return [TimestampHookCreator(arg)]
