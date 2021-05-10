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
# These hooks can be used to run the existing cursor tests on tiered tables.
# They identify tests that create row-store tables and create tiered tables
# instead.  The hook takes an optional argument to specify how many tiers
# to create. The default is 2.
#
# To run with 3 tiers per table:
#     ../test/suite/run.py --hooks tiered=3 cursor
#
# The hooks work with may other tests in the python suite but also encounter
# a variety of failures that I haven't tried to sort out.
from __future__ import print_function

import os, sys, wthooks
import unittest
from wttest import WiredTigerTestCase

# These are the hook functions that are run when particular APIs are called.

# Called to replace Session.create
def session_create_replace(ntiers, orig_session_create, session_self, uri, config):
    if config == None:
        base_config = ""
    else:
        base_config = config

    # If the test is creating a table (not colstore or lsm), create a tiered table instead,
    # using arg to determine number of tiers. Otherwise just do the create as normal.
    #
    # NOTE: The following code uses the old API for creating tiered tables.  As of WT-7173
    # this no longer works.  It will be updated and fixed in WT-7440.
    if (uri.startswith("table:") and "key_format=r" not in base_config and
      "type=lsm" not in base_config):
        tier_string = ""
        for i in range(ntiers):
            new_uri = uri.replace('table:', 'file:tier' + str(i) + '_')
            orig_session_create(session_self, new_uri, config)
            tier_string = tier_string + '"' + new_uri + '", '
        tier_config = 'type=tiered,tiered=(tiers=(' + tier_string[0:-2] + ')),' + base_config
        WiredTigerTestCase.verbose(None, 3,
            'Creating tiered table {} with config = \'{}\''.format(uri, tier_config))
        ret = orig_session_create(session_self, uri, tier_config)
    else:
        ret = orig_session_create(session_self, uri, config)
    return ret

# Called to replace Session.drop
def session_drop_replace(ntiers, orig_session_drop, session_self, uri, config):
    # Drop isn't implemented for tiered tables.  Only do the delete if this could be a
    # uri we created a tiered table for.  Note this isn't a precise match for when we
    # did/didn't create a tiered table, but we don't have the create config around to check.
    ret = 0
    if not uri.startswith("table:"):
        ret = orig_session_drop(session_self, uri, config)
    return ret

# Called to replace Session.verify
def session_verify_replace(ntiers, orig_session_verify, session_self, uri):
    return 0

# Every hook file must have one or more classes descended from WiredTigerHook
# This is where the hook functions are 'hooked' to API methods.
class TieredHookCreator(wthooks.WiredTigerHookCreator):
    def __init__(self, ntiers=0):
        # Argument specifies the number of tiers to test. The default is 2.
        if ntiers == None:
            self.ntiers = 2
        else:
            self.ntiers = int(ntiers)

    # Is this test one we should skip? We skip tests of features supported on standard
    # tables but not tiered tables, specififically cursor caching and checkpoint cursors.
    def skip_test(self, test):
        skip = ["bulk_backup",
                "checkpoint",
                "test_cursor13_big",
                "test_cursor13_drops",
                "test_cursor13_dup",
                "test_cursor13_reopens"]
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
        orig_session_create = self.Session['create']
        self.Session['create'] =  (wthooks.HOOK_REPLACE, lambda s, uri, config:
          session_create_replace(self.ntiers, orig_session_create, s, uri, config))

        orig_session_drop = self.Session['drop']
        self.Session['drop'] = (wthooks.HOOK_REPLACE, lambda s, uri, config:
          session_drop_replace(self.ntiers, orig_session_drop, s, uri, config))

        orig_session_verify = self.Session['verify']
        self.Session['verify'] = (wthooks.HOOK_REPLACE, lambda s, uri:
          session_verify_replace(self.ntiers, orig_session_verify, s, uri))

# Every hook file must have a top level initialize function,
# returning a list of WiredTigerHook objects.
def initialize(arg):
    return [TieredHookCreator(arg)]
