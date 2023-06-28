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
#     ../test/suite/run.py --hook tiered cursor
#
from __future__ import print_function

import os, unittest, wthooks
from wttest import WiredTigerTestCase
from helper_tiered import TieredConfigMixin, gen_tiered_storage_sources

# These are the hook functions that are run when particular APIs are called.

# Add the local storage extension whenever we call wiredtiger_open
def wiredtiger_open_tiered(ignored_self, args):

    tiered_storage_sources = gen_tiered_storage_sources()
    testcase = WiredTigerTestCase.currentTestCase()
    extension_name = testcase.getTierStorageSource()

    auth_token = None
    bucket = None
    prefix = None
    valid_storage_source = False
    # Construct the configuration string based on the storage source.
    for storage_details in tiered_storage_sources:
        if (testcase.getTierStorageSource() == storage_details[0]):
            valid_storage_source = True
            if storage_details[1].__contains__("auth_token"):
                auth_token = storage_details[1].get("auth_token")
                assert auth_token != None, "Bucket access keys environment variables are not set"
            if storage_details[1].__contains__("bucket"):
                bucket = storage_details[1].get("bucket")
            if storage_details[1].__contains__("bucket_prefix"):
                prefix = storage_details[1].get("bucket_prefix")

    if valid_storage_source == False:
        raise AssertionError('Invalid storage source passed in the argument.')

    curconfig = args[-1]
    homedir = args[0]

    bucketpath = bucket
    # If there is already tiered storage enabled, we shouldn't enable it here.
    # We might attempt to let the wiredtiger_open complete without alteration,
    # however, we alter several other API methods that would do weird things with
    # a different tiered_storage configuration. So better to skip the test entirely.
    if 'tiered_storage=' in curconfig:
        testcase.skipTest("cannot run tiered hook on a test that already uses tiered storage")

    # Similarly if this test is already set up to run tiered vs non-tiered scenario, let's
    # not get in the way.
    if hasattr(testcase, 'tiered_conn_config'):
        testcase.skipTest("cannot run tiered hook on a test that already includes TieredConfigMixin")

    if 'in_memory=true' in curconfig:
        testcase.skipTest("cannot run tiered hook on a test that is in-memory")

    # Mark this test as readonly, but don't disallow it.  See testcase_is_readonly().
    if 'readonly=true' in curconfig:
        testcase._readonlyTieredTest = True

    if homedir != None:
        bucketpath = os.path.join(homedir, bucket)
    extension_libs = WiredTigerTestCase.findExtension('storage_sources', extension_name)
    if len(extension_libs) == 0:
        raise Exception(extension_name + ' storage source extension not found')

    if not os.path.exists(bucketpath):
        os.mkdir(bucketpath)

    tier_string = ',tiered_storage=(' + \
      'auth_token=%s,' % auth_token + \
      'bucket=%s,' % bucket + \
      'bucket_prefix=%s,' % prefix + \
      'name=%s)' % extension_name

    # Build the extension strings, we'll need to merge it with any extensions
    # already in the configuration.
    ext_string = 'extensions=['
    start = curconfig.find(ext_string)
    if start >= 0:
        end = curconfig.find(']', start)
        if end < 0:
            raise Exception('hook_tiered: bad extensions in config \"%s\"' % curconfig)
        ext_string = curconfig[start: end]

    tier_string += ',' + ext_string + ',\"%s\"]' % extension_libs[0]

    args = list(args)         # convert from a readonly tuple to a writeable list
    args[-1] += tier_string   # Modify the list

    WiredTigerTestCase.verbose(None, 3,
        '    Calling wiredtiger_open with config = \'{}\''.format(args))

    return args

# We want readonly tests to run with tiered storage, since it is possible to do readonly
# operations.  This function is called for two purposes:
#  - when readonly is enabled, we don't want to do flush_tier calls.
#  - normally the hook silently removes other (not supported) calls, like compact/rename/salvage.
#    Except that some tests enable readonly and call these functions, expecting an exception.
#    So for these "modifying" APIs, we want to actually do the operation (but only when readonly).
def testcase_is_readonly():
    testcase = WiredTigerTestCase.currentTestCase()
    return getattr(testcase, '_readonlyTieredTest', False)

def testcase_has_failed():
    testcase = WiredTigerTestCase.currentTestCase()
    return testcase.failed()

# Called to replace Connection.close
# Insert a call to flush_tier before closing connection.
def connection_close_replace(orig_connection_close, connection_self, config):
    # We cannot call flush_tier on a readonly connection.
    # Likewise we should not call flush_tier if the test case has failed,
    # and the connection is being closed at the end of the run after the failure.
    # Otherwise, diagnosing the original failure may be troublesome.
    if not testcase_is_readonly() and not testcase_has_failed():
        s = connection_self.open_session(None)
        s.checkpoint('flush_tier=(enabled,force=true)')
        s.close()

    ret = orig_connection_close(connection_self, config)
    return ret

# Called to replace Session.checkpoint.
# We add a call to flush_tier after the checkpoint to make sure we are exercising tiered
# functionality.
def session_checkpoint_replace(orig_session_checkpoint, session_self, config):
    # We cannot call flush_tier on a readonly connection.
    if not testcase_is_readonly():
        config += ',flush_tier=(enabled,force=true)'
    return orig_session_checkpoint(session_self, config)

# Called to replace Session.compact
def session_compact_replace(orig_session_compact, session_self, uri, config):
    # FIXME-PM-2538
    # Compact isn't implemented for tiered tables.  Only call it if this can't be the uri
    # of a tiered table.  Note this isn't a precise match for when we did/didn't create
    # a tiered table, but we don't have the create config around to check.
    # We want readonly connections to do the real call, see comment in testcase_is_readonly.
    ret = 0
    if not uri.startswith("table:") or testcase_is_readonly():
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
    # We want readonly connections to do the real call, see comment in testcase_is_readonly.
    # FIXME-WT-9832 Column store testing should be allowed with this hook.
    if not uri.startswith("table:") or "key_format=r" in new_config or "type=lsm" in new_config or testcase_is_readonly():
        new_config = new_config + ',tiered_storage=(name=none)'

    WiredTigerTestCase.verbose(None, 3,
        '    Creating \'{}\' with config = \'{}\''.format(uri, new_config))
    ret = orig_session_create(session_self, uri, new_config)
    return ret

# FIXME-WT-9785
# Called to replace Session.open_cursor. This is needed to skip tests that
# do statistics on (tiered) table data sources, as that is not yet supported.
def session_open_cursor_replace(orig_session_open_cursor, session_self, uri, dupcursor, config):
    if uri != None and (uri.startswith("statistics:table:") or uri.startswith("statistics:file:")):
        testcase = WiredTigerTestCase.currentTestCase()
        testcase.skipTest("statistics on tiered tables not yet implemented")
    if uri != None and uri.startswith("backup:"):
        testcase = WiredTigerTestCase.currentTestCase()
        testcase.skipTest("backup on tiered tables not yet implemented")
    return orig_session_open_cursor(session_self, uri, dupcursor, config)

# Called to replace Session.rename
def session_rename_replace(orig_session_rename, session_self, uri, newuri, config):
    # Rename isn't implemented for tiered tables.  Only call it if this can't be the uri
    # of a tiered table.  Note this isn't a precise match for when we did/didn't create
    # a tiered table, but we don't have the create config around to check.
    # We want readonly connections to do the real call, see comment in testcase_is_readonly.
    ret = 0
    if not uri.startswith("table:") or testcase_is_readonly():
        ret = orig_session_rename(session_self, uri, newuri, config)
    return ret

# Called to replace Session.salvage
def session_salvage_replace(orig_session_salvage, session_self, uri, config):
    # Salvage isn't implemented for tiered tables.  Only call it if this can't be the uri
    # of a tiered table.  Note this isn't a precise match for when we did/didn't create
    # a tiered table, but we don't have the create config around to check.
    # We want readonly connections to do the real call, see comment in testcase_is_readonly.
    ret = 0
    if not uri.startswith("table:") or testcase_is_readonly():
        ret = orig_session_salvage(session_self, uri, config)
    return ret

# Called to replace Session.verify
def session_verify_replace(orig_session_verify, session_self, uri, config):
    # Verify isn't implemented for tiered tables.  Only call it if this can't be the uri
    # of a tiered table.  Note this isn't a precise match for when we did/didn't create
    # a tiered table, but we don't have the create config around to check.
    # We want readonly connections to do the real call, see comment in testcase_is_readonly.
    ret = 0
    if not uri.startswith("table:") or testcase_is_readonly():
        ret = orig_session_verify(session_self, uri, config)
    return ret

# Every hook file must have one or more classes descended from WiredTigerHook
# This is where the hook functions are 'hooked' to API methods.
class TieredHookCreator(wthooks.WiredTigerHookCreator):
    def __init__(self, arg=0):
        # Caller can specify an optional command-line argument.  We're not using it
        # now, but this is where it would show up.

        # Override some platform APIs
        self.platform_api = TieredPlatformAPI(arg)

    # Is this test one we should skip?
    def skip_test(self, test):
        # Skip any test that contains one of these strings as a substring
        skip = ["backup",               # Can't backup a tiered table
                "env01",                # Using environment variable to set WT home
                "config02",             # Using environment variable to set WT home
                "cursor13_ckpt",        # Checkpoint tests with cached cursors
                "cursor13_dup",         # More cursor cache tests
                "cursor13_reopens",     # More cursor cache tests
                "inmem",                # In memory tests don't make sense with tiered storage
                "lsm",                  # If the test name tells us it uses lsm ignore it
                "modify_smoke_recover", # Copying WT dir doesn't copy the bucket directory
                "salvage01",            # Salvage tests directly name files ending in ".wt"
                "test_config_json",     # create replacement can't handle a json config string
                "test_cursor_big",      # Cursor caching verified with stats
                "tiered",               # Tiered tests already do tiering.
                "test_verify",          # Verify not supported on tiered tables (yet)

                # FIXME-WT-9809 The following failures should be triaged and potentially
                # individually reticketed.

                # This first group currently cause severe errors, where Python crashes,
                # whether from internal assertion or other causes.
                "test_bug003.test_bug003",   # Triggers WT-9954
                "test_bug024.test_bug024",
                "test_bulk01.test_bulk_load",   # Triggers WT-9954
                "test_durable_ts03.test_durable_ts03",
                "test_rollback_to_stable20.test_rollback_to_stable",
                "test_stat_log01_readonly.test_stat_log01_readonly",
                "test_stat_log02.test_stats_log_on_json_with_tables",
                "test_txn02.test_ops",
                "test_upgrade.test_upgrade",

                # This group fail within Python for various, sometimes unknown, reasons.
                "test_bug018.test_bug018",
                "test_checkpoint.test_checkpoint",
                "test_checkpoint_snapshot02.test_checkpoint_snapshot_with_txnid_and_timestamp",
                "test_compat05.test_compat05",
                "test_config05.test_too_many_sessions",
                "test_config09.test_config09",
                "test_drop.test_drop",
                "test_empty.test_empty",     # looks at wt file names and uses column store
                "test_encrypt06.test_encrypt",
                "test_encrypt07.test_salvage_api",
                "test_encrypt07.test_salvage_api_damaged",
                "test_encrypt07.test_salvage_process_damaged",
                "test_export01.test_export_restart",
                "test_hs21.test_hs",
                "test_import04.test_table_import",
                "test_import09.test_import_table_repair",
                "test_import09.test_import_table_repair",
                "test_import11.test_file_import",
                "test_import11.test_file_import",
                "test_join03.test_join",
                "test_join07.test_join_string",
                "test_jsondump02.test_json_all_bytes",
                "test_metadata_cursor02.test_missing",
                "test_prepare02.test_prepare_session_operations",
                "test_prepare_hs03.test_prepare_hs",
                "test_prepare_hs03.test_prepare_hs",
                "test_rename.test_rename",
                "test_rollback_to_stable09.test_rollback_to_stable",
                "test_rollback_to_stable28.test_update_restore_evict_recovery",
                "test_rollback_to_stable34.test_rollback_to_stable",
                "test_rollback_to_stable35.test_rollback_to_stable",
                "test_rollback_to_stable36.test_rollback_to_stable",
                "test_sweep03.test_disable_idle_timeout_drop",
                "test_sweep03.test_disable_idle_timeout_drop_force",
                "test_truncate01.test_truncate_cursor",
                "test_truncate01.test_truncate_cursor_end",
                "test_truncate01.test_truncate_timestamp",
                "test_truncate01.test_truncate_uri",
                "test_truncate10.test_truncate10",
                "test_truncate12.test_truncate12",
                "test_truncate13.test_truncate",
                "test_truncate14.test_truncate",
                "test_truncate16.test_truncate16",
                "test_truncate18.test_truncate18",
                "test_truncate15.test_truncate15",
                "test_truncate19.test_truncate19",
                "test_truncate20.test_truncate20",
                "test_txn22.test_corrupt_meta",
                "test_verbose01.test_verbose_single",
                "test_verbose02.test_verbose_single",
                "test_verify2.test_verify_ckpt",
                ]

        for item in skip:
            if item in str(test):
                return True
        return False

    # Remove tests that won't work on tiered cursors
    def filter_tests(self, tests):
        new_tests = unittest.TestSuite()
        new_tests.addTests([t for t in tests if not self.skip_test(t)])
        return new_tests

    def get_platform_api(self):
        return self.platform_api

    def setup_hooks(self):
        orig_connection_close = self.Connection['close']
        self.Connection['close'] = (wthooks.HOOK_REPLACE, lambda s, config=None:
          connection_close_replace(orig_connection_close, s, config))

        orig_session_compact = self.Session['compact']
        self.Session['compact'] =  (wthooks.HOOK_REPLACE, lambda s, uri, config=None:
          session_compact_replace(orig_session_compact, s, uri, config))

        orig_session_create = self.Session['create']
        self.Session['create'] =  (wthooks.HOOK_REPLACE, lambda s, uri, config=None:
          session_create_replace(orig_session_create, s, uri, config))

        orig_session_open_cursor = self.Session['open_cursor']
        self.Session['open_cursor'] = (wthooks.HOOK_REPLACE, lambda s, uri, todup=None, config=None:
          session_open_cursor_replace(orig_session_open_cursor, s, uri, todup, config))

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

# Override some platform APIs for this hook.
class TieredPlatformAPI(wthooks.WiredTigerHookPlatformAPI):
    def __init__(self, arg=None):
        self.tier_share_percent = 0
        self.tier_cache_percent = 0
        self.tier_storage_source = 'dir_store'
        params = []
        if arg:
            params = [config.split('=') for config in arg.split(',')]

        for param_key, param_value in params :
            if param_key == 'tier_populate_share':
                self.tier_share_percent = int(param_value)
            elif param_key == 'tier_populate_cache':
                self.tier_cache_percent = int(param_value)
            elif param_key == 'tier_storage_source':
                self.tier_storage_source = param_value

    def tableExists(self, name):
        for i in range(1, 9):
            tablename = name + "-000000000{}.wtobj".format(i)
            if os.path.exists(tablename):
                return True
        return False

    def initialFileName(self, uri):
        if uri.startswith('table:'):
            return uri[6:] + '-0000000001.wtobj'
        else:
            return wthooks.DefaultPlatformAPI.initialFileName(uri)

    def getTierSharePercent(self):
        return self.tier_share_percent

    def getTierCachePercent(self):
        return self.tier_cache_percent

    def getTierStorageSource(self):
        return self.tier_storage_source

# Every hook file must have a top level initialize function,
# returning a list of WiredTigerHook objects.
def initialize(arg):
    return [TieredHookCreator(arg)]
