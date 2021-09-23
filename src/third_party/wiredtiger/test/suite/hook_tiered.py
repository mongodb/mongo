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

# hook_tiered.py
#
# Substitute tiered tables for regular (row-store) tables in Python tests.
#
# These hooks are intended for basic testing of tiered tables.  There are several
# pieces of functionality here.
#
# 1. We add tiered storage parameters to the config when calling wiredtiger_open()
#
# 2. If we create an object that is *not* a table:, we add options to its config
#    so that it will be stored local-only.  Tiered storage isn't intended (yet?) for
#    use with lsm or column store.
#
# 3. We add calls to flush_tier().  Currently we only flush after a checkpoint() call,
#    but we should add others.
#
# 4. We stub out some functions that aren't supported by tiered tables.  This will
#    break tests of those functions.  But often when they are used in other tests, we
#    can get away with returning success without performing the operation.
#
# To run, for example, the cursor tests with these hooks enabled:
#     ../test/suite/run.py --hooks tiered cursor
#
from __future__ import print_function

import os, sys, wthooks
import unittest
from wttest import WiredTigerTestCase

# These are the hook functions that are run when particular APIs are called.

# Add the local storage extension whenever we call wiredtiger_open
def wiredtiger_open_tiered(ignored_self, args):
    auth_token = "test_token"
    bucket = "mybucket"
    extension_name = "local_store"
    prefix = "pfx-"
    extension_libs = WiredTigerTestCase.findExtension('storage_sources', extension_name)
    if len(extension_libs) == 0:
        raise Exception(extension_name + ' storage source extension not found')

    if not os.path.exists(bucket):
        os.mkdir(bucket)
    tier_string = ',tiered_storage=(auth_token=%s,' % auth_token + \
      'bucket=%s,' % bucket + \
      'bucket_prefix=%s,' % prefix + \
      'name=%s),tiered_manager=(wait=0),' % extension_name + \
      'extensions=[\"%s\"],' % extension_libs[0]

    args = list(args)         # convert from a readonly tuple to a writeable list
    args[-1] += tier_string   # Modify the list

    WiredTigerTestCase.verbose(None, 3,
        '    Calling wiredtiger_open with config = \'{}\''.format(args))

    return args

# Called to replace Connection.close
# Insert a call to flush_tier before closing connection.
def connection_close_replace(orig_connection_close, connection_self, config):
    s = connection_self.open_session(None)
    s.flush_tier(None)
    s.close()
    ret = orig_connection_close(connection_self, config)
    return ret

# Called to replace Session.alter
def session_alter_replace(orig_session_alter, session_self, uri, config):
    # Alter isn't implemented for tiered tables.  Only call it if this can't be the uri
    # of a tiered table.  Note this isn't a precise match for when we did/didn't create
    # a tiered table, but we don't have the create config around to check.
    ret = 0
    if not uri.startswith("table:"):
        ret = orig_session_alter(session_self, uri, config)
    return ret

# Called to replace Session.checkpoint.
# We add a call to flush_tier after the checkpoint to make sure we are exercising tiered
# functionality.
def session_checkpoint_replace(orig_session_checkpoint, session_self, config):
    ret = orig_session_checkpoint(session_self, config)
    if ret != 0:
        return ret
    WiredTigerTestCase.verbose(None, 3,
        '    Calling flush_tier() after checkpoint')
    return session_self.flush_tier(None)

# Called to replace Session.compact
def session_compact_replace(orig_session_compact, session_self, uri, config):
    # Compact isn't implemented for tiered tables.  Only call it if this can't be the uri
    # of a tiered table.  Note this isn't a precise match for when we did/didn't create
    # a tiered table, but we don't have the create config around to check.
    ret = 0
    if not uri.startswith("table:"):
        ret = orig_session_compact(session_self, uri, config)
    return ret

# Called to replace Session.create
def session_create_replace(orig_session_create, session_self, uri, config):
    if config == None:
        new_config = ""
    else:
        new_config = config

    # If the test isn't creating a table (i.e., it's a column store or lsm) create it as a
    # "local only" object.  Otherwise we get tiered storage from the connection defaults.
    if not uri.startswith("table:") or "key_format=r" in new_config or "type=lsm" in new_config:
        new_config = new_config + ',tiered_storage=(name=none)'

    WiredTigerTestCase.verbose(None, 3,
        '    Creating \'{}\' with config = \'{}\''.format(uri, new_config))
    ret = orig_session_create(session_self, uri, new_config)
    return ret

# Called to replace Session.drop
def session_drop_replace(orig_session_drop, session_self, uri, config):
    # Drop isn't implemented for tiered tables.  Only call it if this can't be the uri
    # of a tiered table.  Note this isn't a precise match for when we did/didn't create
    # a tiered table, but we don't have the create config around to check.
    ret = 0
    if not uri.startswith("table:"):
        ret = orig_session_drop(session_self, uri, config)
    return ret

# Called to replace Session.rename
def session_rename_replace(orig_session_rename, session_self, uri, newuri, config):
    # Rename isn't implemented for tiered tables.  Only call it if this can't be the uri
    # of a tiered table.  Note this isn't a precise match for when we did/didn't create
    # a tiered table, but we don't have the create config around to check.
    ret = 0
    if not uri.startswith("table:"):
        ret = orig_session_rename(session_self, uri, newuri, config)
    return ret

# Called to replace Session.salvage
def session_salvage_replace(orig_session_salvage, session_self, uri, config):
    # Salvage isn't implemented for tiered tables.  Only call it if this can't be the uri
    # of a tiered table.  Note this isn't a precise match for when we did/didn't create
    # a tiered table, but we don't have the create config around to check.
    ret = 0
    if not uri.startswith("table:"):
        ret = orig_session_salvage(session_self, uri, config)
    return ret

# Called to replace Session.verify
def session_verify_replace(orig_session_verify, session_self, uri, config):
    # Verify isn't implemented for tiered tables.  Only call it if this can't be the uri
    # of a tiered table.  Note this isn't a precise match for when we did/didn't create
    # a tiered table, but we don't have the create config around to check.
    ret = 0
    if not uri.startswith("table:"):
        ret = orig_session_verify(session_self, uri, config)
    return ret

# Every hook file must have one or more classes descended from WiredTigerHook
# This is where the hook functions are 'hooked' to API methods.
class TieredHookCreator(wthooks.WiredTigerHookCreator):
    def __init__(self, arg=0):
        # Caller can specify an optional command-line argument.  We're not using it
        # now, but this is where it would show up.
        return

    # Is this test one we should skip?
    def skip_test(self, test):
        # Skip any test that contains one of these strings as a substring
        skip = ["backup",               # Can't backup a tiered table
                "cursor13_ckpt",        # Checkpoint tests with cached cursors
                "cursor13_drops",       # Tests that require working drop implementation
                "cursor13_dup",         # More cursor cache tests
                "cursor13_reopens",     # More cursor cache tests
                "lsm",                  # If the test name tells us it uses lsm ignore it
                "modify_smoke_recover", # Copying WT dir doesn't copy the bucket directory
                "test_config_json",     # create replacement can't handle a json config string
                "test_cursor_big",      # Cursor caching verified with stats
                "tiered"]
        for item in skip:
            if item in str(test):
                return True
        return False

    # Remove tests that won't work on tiered cursors
    def filter_tests(self, tests):
        new_tests = unittest.TestSuite()
        new_tests.addTests([t for t in tests if not self.skip_test(t)])
        return new_tests

    def setup_hooks(self):
        orig_connection_close = self.Connection['close']
        self.Connection['close'] = (wthooks.HOOK_REPLACE, lambda s, config=None:
          connection_close_replace(orig_connection_close, s, config))

        orig_session_alter = self.Session['alter']
        self.Session['alter'] =  (wthooks.HOOK_REPLACE, lambda s, uri, config=None:
          session_alter_replace(orig_session_alter, s, uri, config))

        orig_session_compact = self.Session['compact']
        self.Session['compact'] =  (wthooks.HOOK_REPLACE, lambda s, uri, config=None:
          session_compact_replace(orig_session_compact, s, uri, config))

        orig_session_create = self.Session['create']
        self.Session['create'] =  (wthooks.HOOK_REPLACE, lambda s, uri, config=None:
          session_create_replace(orig_session_create, s, uri, config))

        orig_session_drop = self.Session['drop']
        self.Session['drop'] = (wthooks.HOOK_REPLACE, lambda s, uri, config=None:
          session_drop_replace(orig_session_drop, s, uri, config))

        orig_session_rename = self.Session['rename']
        self.Session['rename'] = (wthooks.HOOK_REPLACE, lambda s, uri, newuri, config=None:
          session_rename_replace(orig_session_rename, s, uri, newuri, config))

        orig_session_salvage = self.Session['salvage']
        self.Session['salvage'] = (wthooks.HOOK_REPLACE, lambda s, uri, config=None:
          session_salvage_replace(orig_session_salvage, s, uri, config))

        orig_session_verify = self.Session['verify']
        self.Session['verify'] = (wthooks.HOOK_REPLACE, lambda s, uri, config=None:
          session_verify_replace(orig_session_verify, s, uri, config))

        self.wiredtiger['wiredtiger_open'] = (wthooks.HOOK_ARGS, wiredtiger_open_tiered)

# Every hook file must have a top level initialize function,
# returning a list of WiredTigerHook objects.
def initialize(arg):
    return [TieredHookCreator(arg)]
