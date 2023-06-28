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

# hook_demo.py
#   Demonstration of hooks.  Run via:
#     python run.py --hook demo=N base01
#
#  These hooks are set up:
#  - alter wiredtiger_open arguments (in a benign way)
#  - report after wiredtiger_open is called.
#  - notify on session.open_cursor
#  - intercept the session.create call
#
#   With N == 0, the session.create call reports its arguments and calls original session.create.
#   with N == 1, it does an additional session.drop call (which should cause tests to fail);
#   with N == 2, it does an additional session.create after the drop call (which should work).
#
#   Note that notify hooks don't have to simply report, they can call other methods,
#   set attributes on objects, etc.  For example, one can save the open_cursor
#   config string as an attribute on the cursor object, and examine it in another
#   hooked method.
from __future__ import print_function

import os, sys, wthooks
from wttest import WiredTigerTestCase

# Print to /dev/tty for debugging, since anything extraneous to stdout/stderr will
# cause a test error.
def tty(s):
    WiredTigerTestCase.tty(s)

# These are the hook functions that are run when particular APIs are called.

# Called to manipulate args for wiredtiger_open
def wiredtiger_open_args(ignored_self, args):
    tty('>>> wiredtiger_open, adding cache_size')
    args = list(args)    # convert from a readonly tuple to a writeable list
    args[-1] += ',,,cache_size=500M,,,'   # modify the last arg
    return args

# Called to notify after successful wiredtiger_open
def wiredtiger_open_notify(ignored_self, ret, *args):
    tty('>>> wiredtiger_open({}) returned {}'.format(args, ret))

# Called to notify after successful Session.open_cursor
def session_open_cursor_notify(self, ret, *args):
    tty('>>> session.open_cursor({}) returned {}, session is {}'.format(args, ret, self))

# Called to replace Session.create
# We do different things (described above) as indicated by our command line argument.
def session_create_replace(arg, orig_session_create, session_self, uri, config):
    tty('>>> session.create({},{}), session is {}'.format(uri, config, session_self))
    if arg == 0:
        # Just do a regular create
        return orig_session_create(session_self, uri, config)
    elif arg == 1:
        # Do a regular create, followed by a drop.  This will cause test failures.
        ret = orig_session_create(session_self, uri, config)
        # We didn't replace drop, so we can call it as a method
        tty('>>> session.drop({})'.format(uri))
        session_self.drop(uri)
        return ret
    elif arg == 2:
        # Do a regular create, followed by a drop, then another create.  Should work.
        ret = orig_session_create(session_self, uri, config)
        # We didn't replace drop, so we can call it as a method
        tty('>>> session.drop({})'.format(uri))
        session_self.drop(uri)
        tty('>>> session.create({},{})'.format(uri, config))
        orig_session_create(session_self, uri, config)
        return ret

# Every hook file must have one or more classes descended from WiredTigerHook
# This is where the hook functions are 'hooked' to API methods.
class DemoHookCreator(wthooks.WiredTigerHookCreator):
    def __init__(self, arg=0):
        # An argument may alter the test
        if arg == None:
            self.arg = 0
        else:
            self.arg = int(arg)

    # We have an opportunity to filter the list of tests to be run.
    # For this demo, we don't filter.
    def filter_tests(self, tests):
        print('Filtering: ' + str(tests))
        return tests

    # If the hook wants to override some implementation of the test framework,
    # it would need to subclass wthooks.WiredTigerHookPlatformAPI and return
    # an object of that type here.
    def get_platform_api(self):
        return None

    def setup_hooks(self):
        tty('>> SETUP HOOKS RUN')
        orig_session_create = self.Session['create']     # gets original function
        self.wiredtiger['wiredtiger_open'] = (wthooks.HOOK_ARGS, wiredtiger_open_args)
        self.wiredtiger['wiredtiger_open'] = (wthooks.HOOK_NOTIFY, wiredtiger_open_notify)
        self.Session['create'] = (wthooks.HOOK_REPLACE, lambda s, uri, config:
          session_create_replace(self.arg, orig_session_create, s, uri, config))
        self.Session['open_cursor'] = (wthooks.HOOK_NOTIFY, session_open_cursor_notify)

# Every hook file must have a top level initialize function,
# returning a list of WiredTigerHook objects.
def initialize(arg):
    return [DemoHookCreator(arg)]
