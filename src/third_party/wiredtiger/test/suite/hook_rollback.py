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

# hook_rollback.py
#   Intentionally have random failures of rollback some fraction of the time:
#     python run.py --hook rollback=FAILRATE base01
#
# FAILRATE should be a floating number less than one, for example, 0.25 to fail a quarter of the time.
#
# This hook is used to test a feature of the test suite, where tests with rollback errors are automatically
# restarted.

from __future__ import print_function

import wthooks, random, wiredtiger
from wttest import WiredTigerTestCase

# Print to /dev/tty for debugging, since anything extraneous to stdout/stderr will
# cause a test error.
def tty(s):
    WiredTigerTestCase.tty(s)

# These are the hook functions that are run when particular APIs are called.

def session_begin_transaction_notify(ret, session, *args):
    session.in_transaction = True
    #tty('==> in txn, session={}'.format(session))

def session_end_transaction_notify(ret, session, *args):
    session.in_transaction = False
    #tty('==> out txn, session={}'.format(session))

# Here we patch a deficiency of our SWIG setup.  When a cursor is created via
# a session.open_cursor() call, a unique Cursor python object is created.
# However, that particular 'cursor' object doesn't know what 'session' object created it,
# only that there is a property 'session' that maps to the WT_CURSOR->session pointer.
# The upshot is that cursor.session will not give us the same Python object that
# created the cursor, though it will be a wrapper around the same C WT_SESSION.
#
# To fix this, we change the Cursor.session property, it was originally managed by SWIG,
# now it just returns a session field, that we set immediately after the open_cursor.
# In an ideal world, we'd change our SWIG configuration to do the right thing for
# Cursor.session and Session.connection, but I don't think any of the regular tests care about it.

wiredtiger.Cursor.session = property(lambda x: x._session_value)
def session_open_cursor_notify(cursor, session, *args):
    cursor._session_value = session

def cursor_notify_for_rollback(cursor, creator):
    s = cursor.session
    #tty('   cursor_notify_for_rollback, session={}'.format(s))

    # We only want to trigger a rollback error when we are in a transaction,
    # to mimic when these errors occur in actual WiredTiger calls.  To do this
    # we have a field on the session object that tells us if we are in a
    # transaction.
    if (hasattr(s, 'in_transaction') and s.in_transaction):
        if creator.do_retry():
            # Add something to stdout, we want to make sure it is cleaned up on a test retry.
            print('retry transaction rollback error')
            raise wiredtiger.WiredTigerRollbackError('retryable rollback error')

# Every hook file must have one or more classes descended from WiredTigerHook
# This is where the hook functions are 'hooked' to API methods.
class RollbackHookCreator(wthooks.WiredTigerHookCreator):
    def __init__(self, arg=0):
        # The argument is the fail rate.  From it, we compute an integral "mod"
        if arg == None:
            failrate = 0.10
        else:
            failrate = float(arg)
        if failrate < 0.0 or failrate > 1.0:
            raise Exception('Illegal arg {} to rollback hook creator, expect 0.0 < N <= 1.0'.
              format(failrate))
        self.mod = int(1 / failrate)
        self.rand = random.Random()
        self.platform_api = wthooks.DefaultPlatformAPI()

    def get_platform_api(self):
        return self.platform_api

    def do_retry(self):
        return self.rand.randint(1, 1000000) % self.mod == 0

    # No filtering needed
    def filter_tests(self, tests):
        #print('Filtering: ' + str(tests))
        return tests

    def setup_hooks(self):
        self.Session['begin_transaction'] = (wthooks.HOOK_NOTIFY, session_begin_transaction_notify)
        self.Session['commit_transaction'] = (wthooks.HOOK_NOTIFY, session_end_transaction_notify)
        self.Session['rollback_transaction'] = (wthooks.HOOK_NOTIFY, session_end_transaction_notify)
        self.Session['open_cursor'] = (wthooks.HOOK_NOTIFY, session_open_cursor_notify)

        self.Cursor['insert'] = (wthooks.HOOK_NOTIFY,
            lambda ret, c, *args, **kw: cursor_notify_for_rollback(c, self))
        self.Cursor['modify'] = (wthooks.HOOK_NOTIFY,
            lambda ret, c, *args, **kw: cursor_notify_for_rollback(c, self))
        self.Cursor['search'] = (wthooks.HOOK_NOTIFY,
            lambda ret, c, *args, **kw: cursor_notify_for_rollback(c, self))
        self.Cursor['search_near'] = (wthooks.HOOK_NOTIFY,
            lambda ret, c, *args, **kw: cursor_notify_for_rollback(c, self))
        self.Cursor['update'] = (wthooks.HOOK_NOTIFY,
            lambda ret, c, *args, **kw: cursor_notify_for_rollback(c, self))

# Every hook file must have a top level initialize function,
# returning a list of WiredTigerHook objects.
def initialize(arg):
    return [RollbackHookCreator(arg)]
