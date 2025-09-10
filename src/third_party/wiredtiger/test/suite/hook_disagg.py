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

# hook_disagg.py
#
# Substitute layered tables for regular (row-store) tables in Python tests.
#
# These hooks are intended for basic testing.  There are several
# pieces of functionality here.
#
# 1. We add disaggregated storage parameters to the config when calling wiredtiger_open()
#
# 2. When creating a table, we add options to use the page_log service.
#
# 3. We stub out some functions that aren't supported by disaggregated storage.
#
# To run, for example, the cursor tests with these hooks enabled:
#     ../test/suite/run.py --hook disagg cursor
#
from __future__ import print_function

import os, re, unittest, wthooks, wttest
from wttest import WiredTigerTestCase
from helper_disagg import DisaggConfigMixin, gen_disagg_storages, disagg_ignore_expected_output

# These are the hook functions that are run when particular APIs are called.

# Add the local storage extension whenever we call wiredtiger_open
def wiredtiger_open_replace(orig_wiredtiger_open, homedir, conn_config):

    disagg_storages = gen_disagg_storages()
    testcase = WiredTigerTestCase.currentTestCase()
    platform_api = testcase.platform_api
    disagg_parameters = platform_api.getDisaggParameters()
    page_log_name = disagg_parameters.page_log
    page_log_config = disagg_parameters.config

    valid_page_log = False
    # Construct the configuration string based on the storage source.
    for disagg_details in disagg_storages:
        if (testcase.getDisaggParameters().page_log == disagg_details[0]):
            valid_page_log = True

    if valid_page_log == False:
        raise AssertionError('Invalid page_log passed in the argument.')

    # If there is already disagg storage enabled, we shouldn't enable it here.
    # We might attempt to let the wiredtiger_open complete without alteration,
    # however, we alter several other API methods that would do weird things with
    # a different disagg_storage configuration. So better to skip the test entirely.
    if 'disaggregated=' in conn_config:
        skip_test("cannot run disagg hook on a test that already uses disagg storage")

    # Similarly if this test is already set up to run disagg vs non-disagg scenario, let's
    # not get in the way.
    if hasattr(testcase, 'disagg_conn_config'):
        skip_test("cannot run disagg hook on a test that already includes DisaggConfigMixin")

    if 'in_memory=true' in conn_config:
        skip_test("cannot run disagg hook on a test that is in-memory")

    if 'compatibility=' in conn_config:
        skip_test("cannot run disagg hook on a test that requires compatibility in the config string")

    if 'tiered_storage=' in conn_config:
        skip_test("cannot run disagg hook on a test that uses tiered_storage in the config string")


    extension_libs = WiredTigerTestCase.findExtension('page_log', page_log_name)
    if len(extension_libs) == 0:
        raise Exception(extension_name + ' storage source extension not found')

    WiredTigerTestCase.verbose(None, 3, f'role={disagg_parameters.role}')
    disagg_config = ',verbose=[layered]' \
        + f',disaggregated=(role="{disagg_parameters.role}"' \
        + f',page_log={page_log_name})'

    # Mark the test case whether the connection is a disagg leader or follower.
    # This is used for the initial state.
    #
    # We'd really like to mark this on the connection object itself, since there may well be
    # multiple connections in a test (and we might want them to have different roles in the future).
    # But marking the connection object with a regular attribute does not work with SWIG.
    #
    # To elaborate, if we set conn.foo = 1, and later open a session and cursors, then from a cursor
    # intercept function, we can get the connection via:
    #   myconn = cursor.session.connection
    # Then myconn will be an entirely new Python object from the original conn, but both are
    # backed by the same connection. In other words, myconn is created on the fly by SWIG, and
    # it won't have the foo attribute we originally set.
    #
    # This doesn't matter too much in this case, because we're usually hooking into tests with
    # a single connection.
    testcase.disagg_leader = (disagg_parameters.role == "leader")

    # Build the extension strings, we'll need to merge it with any extensions
    # already in the configuration.
    ext_string = 'extensions=['
    start = conn_config.find(ext_string)
    if start >= 0:
        end = conn_config.find(']', start)
        if end < 0:
            raise Exception('hook_disagg: bad extensions in config \"%s\"' % conn_config)
        ext_string = conn_config[start: end]

    if page_log_config == None:
        ext_lib = '\"%s\"' % extension_libs[0]
    else:
        ext_lib = '\"%s\"=(config=\"%s\")' % (extension_libs[0], page_log_config)

    disagg_config += ',' + ext_string + ',%s]' % ext_lib

    config = conn_config + disagg_config

    WiredTigerTestCase.verbose(None, 3, f'    Calling wiredtiger_open({homedir}, {config})')

    result = orig_wiredtiger_open(homedir, config)
    # Disaggregated storage generates some extra verbose output which must be ignored.
    disagg_ignore_expected_output(testcase)

    return result

def testcase_has_failed():
    testcase = WiredTigerTestCase.currentTestCase()
    return testcase.failed()

def testcase_has_skipped():
    testcase = WiredTigerTestCase.currentTestCase()
    return testcase.skipped

def skip_test(comment):
    testcase = WiredTigerTestCase.currentTestCase()
    testcase.skipTest(comment)

def mark_as_layered(uri):
    testcase = WiredTigerTestCase.currentTestCase()
    testcase.layered_uris.add(uri)

def marked_as_non_layered(uri):
    testcase = WiredTigerTestCase.currentTestCase()
    return uri in testcase.non_layered_uris

def is_layered(uri):
    testcase = WiredTigerTestCase.currentTestCase()
    return uri in testcase.layered_uris

# If the uri has been marked as layered, then transform to a layered uri
def replace_uri(uri):
    testcase = WiredTigerTestCase.currentTestCase()
    disagg_parameters = testcase.platform_api.getDisaggParameters()
    # Handle statistics: or statistics:<uri>
    stat_prefix = 'statistics:'
    if uri.startswith(stat_prefix):
        return 'statistics:' + replace_uri(uri[len(stat_prefix):])
    if is_layered(uri) and disagg_parameters.table_prefix == "layered":
        return uri.replace("table:", "layered:")
    else:
        return uri

# Called to replace Session.alter.
def session_alter_replace(orig_session_open_cursor, session_self, uri, config):
    uri = replace_uri(uri)
    return orig_session_open_cursor(session_self, uri, config)

# Called to replace Session.checkpoint.
# We add a call to flush_tier during every checkpoint to make sure we are exercising disagg
# functionality.
def session_checkpoint_replace(orig_session_checkpoint, session_self, config):
    # We cannot do named checkpoints with disagg storage objects.
    # We can't really continue the test without the name, as the name will certainly be used.
    if config == None:
        config = ''
    if 'name=' in config:
        skip_test('named checkpoints do not work in disagg storage')
    return orig_session_checkpoint(session_self, config)

# Called to replace Session.compact
def session_compact_replace(orig_session_compact, session_self, uri, config):
    uri = replace_uri(uri)
    return orig_session_compact(session_self, uri, config)

# Called to replace Session.create
def session_create_replace(orig_session_create, session_self, uri, config):
    testcase = WiredTigerTestCase.currentTestCase()
    disagg_parameters = testcase.platform_api.getDisaggParameters()

    # If the test isn't creating a table (i.e., it's a column store or lsm) create it as a
    # regular (not layered) object.  Otherwise we get disagg storage from the connection defaults.
    if uri.startswith("table:") \
       and not 'colgroups=' in config \
       and not 'import=' in config \
       and not 'key_format=r' in config \
       and not 'type=lsm' in config \
       and not marked_as_non_layered(uri):
        mark_as_layered(uri)
        if (disagg_parameters.table_prefix == "layered"):
            WiredTigerTestCase.verbose(None, 1, f'    Replacing, old uri = "{uri}"')
            uri = replace_uri(uri)
            WiredTigerTestCase.verbose(None, 1, f'    Replacing, new uri = "{uri}"')
        else:
            WiredTigerTestCase.verbose(None, 1, f'    Replacing, old config = "\{config}"')
            config += ',block_manager=disagg,type=layered'
            WiredTigerTestCase.verbose(None, 1, f'    Replacing, new config = "\{config}"')

    # If this is an index create and the main table was already tagged to be layered,
    # there's nothing we can do to "fix" it.  Currently "index:foo" is hardwired to
    # link up with "table:foo", and there is not a "table:foo", only a "layered:foo".
    WiredTigerTestCase.verbose(None, 1, f'    Creating "{uri}" with config = "{config}"')

    # Check if log table is enabled at connection level. If it is, by default session will create a log table unless explicitly disabled in session config.
    # Skip test if it is enabled
    # FIXME-WT-15221 Should throw an error when this is set in disagg"
    conn_config = testcase.conn_config
    if hasattr(conn_config, '__call__'):
        conn_config = testcase.conn_config()
    log_enabled = re.search(r'log=\(enabled(?:=true|,|\))', conn_config) is not None
    if log_enabled and 'log=(enabled=false' not in config:
        skip_test("Log tables are not supported in disagg.")

    if uri.startswith("index:"):
        # URI is index:base_name:index_name
        last_colon = uri.rfind(':')
        base_uri = 'table:' + uri[6:last_colon]
        WiredTigerTestCase.verbose(None, 1, f'    BaseURI "{base_uri}", {last_colon}')

        testcase = WiredTigerTestCase.currentTestCase()
        WiredTigerTestCase.verbose(None, 1, f'    Layered URIS: "{testcase.layered_uris}"')

        if is_layered(base_uri):
            WiredTigerTestCase.verbose(None, 1, f'    SKIPPING "{base_uri}"')
            skip_test('indices do not work in disagg storage')

    WiredTigerTestCase.verbose(None, 3, f'    Creating "{uri}" with config = "{config}"')
    ret = orig_session_create(session_self, uri, config)
    return ret

# Called to replace Session.open_cursor.  We skip calls that do backup
# as that is not yet supported in disaggregated storage.
def session_open_cursor_replace(orig_session_open_cursor, session_self, uri, dupcursor, config):
    if uri != None and uri.startswith("backup:"):
        skip_test("backup on disagg tables not yet implemented")
    uri = replace_uri(uri)
    return orig_session_open_cursor(session_self, uri, dupcursor, config)

# Called to replace Session.salvage
def session_salvage_replace(orig_session_salvage, session_self, uri, config):
    uri = replace_uri(uri)
    return orig_session_salvage(session_self, uri, config)

# Called to replace Session.truncate.
def session_truncate_replace(orig_session_truncate, session_self, uri, start, stop, config):
    #uri = replace_uri(uri)
    #return orig_session_truncate(session_self, uri, start, stop, config)
    skip_test("truncate on disagg tables not yet implemented")

# Called to replace Session.verify
def session_verify_replace(orig_session_verify, session_self, uri, config):
    uri = replace_uri(uri)
    return orig_session_verify(session_self, uri, config)

# Every hook file must have one or more classes descended from WiredTigerHook
# This is where the hook functions are 'hooked' to API methods.
class DisaggHookCreator(wthooks.WiredTigerHookCreator):
    def __init__(self, arg=0):
        # Caller can specify an optional command-line argument.  We're not using it
        # now, but this is where it would show up.

        # Override some platform APIs
        self.platform_api = DisaggPlatformAPI(arg)

    # Determine whether a test should be skipped, if it should also return the reason for skipping.
    # Some features aren't supported with disagg storage currently. If they exist in
    # the test name (or scenario name) skip the test.
    def should_skip(self, test) -> (bool, str):
        skip_categories = [
            ("disagg",               "Disagg tests already turn on the proper stuff"),
            ("inmem",                "In memory tests don't make sense with disagg storage"),
            ("layered",              "Layered tests already turn on the proper stuff"),
            ("live_restore",         "Live restore is not supported with disagg storage"),
            ("lsm",                  "LSM is not supported with tiering"),
            ("modify_smoke_recover", "Copying WT dir doesn't copy the PALM directory"),
            ("rollback_to_stable",   "Rollback to stable is not needed at startup"),
            ("test_backup",          "Can't backup a disagg table"),
            ("test_compact",         "Can't compact a disagg table"),
            ("test_config_json",     "Disagg hook's create function can't handle a json config string"),
            ("test_cursor_big",      "Cursor caching verified with stats"),
            ("test_cursor_bound",    "Can't use cursor bounds with a disagg table"),
            ("test_salvage",         "Salvage tests directly name files ending in '.wt'"),
            ("test_truncate",        "Truncate on disagg tables not yet implemented"),
            ("tiered",               "Tiered tests do not apply to disagg"),
        ]

        for (skip_string, skip_reason) in skip_categories:
            if skip_string in str(test):
                return (True, skip_reason)

        return (False, None)

    # Skip tests that won't work on disagg cursors
    def register_skipped_tests(self, tests):
        for t in tests:
            (should_skip, skip_reason) = self.should_skip(t)
            if should_skip:
                wttest.register_skipped_test(t, "disagg", skip_reason)

    def get_platform_api(self):
        return self.platform_api

    def setup_hooks(self):
        orig_session_alter = self.Session['alter']
        self.Session['alter'] =  (wthooks.HOOK_REPLACE, lambda s, uri, config=None:
            session_alter_replace(orig_session_alter, s, uri, config))

        orig_session_checkpoint = self.Session['checkpoint']
        self.Session['checkpoint'] =  (wthooks.HOOK_REPLACE, lambda s, config=None:
            session_checkpoint_replace(orig_session_checkpoint, s, config))

        orig_session_compact = self.Session['compact']
        self.Session['compact'] =  (wthooks.HOOK_REPLACE, lambda s, uri, config=None:
          session_compact_replace(orig_session_compact, s, uri, config))

        orig_session_create = self.Session['create']
        self.Session['create'] =  (wthooks.HOOK_REPLACE, lambda s, uri, config=None:
          session_create_replace(orig_session_create, s, uri, config))

        orig_session_open_cursor = self.Session['open_cursor']
        self.Session['open_cursor'] = (wthooks.HOOK_REPLACE, lambda s, uri, todup=None, config=None:
          session_open_cursor_replace(orig_session_open_cursor, s, uri, todup, config))

        orig_session_salvage = self.Session['salvage']
        self.Session['salvage'] = (wthooks.HOOK_REPLACE, lambda s, uri, config=None:
          session_salvage_replace(orig_session_salvage, s, uri, config))

        orig_session_truncate = self.Session['truncate']
        self.Session['truncate'] = (wthooks.HOOK_REPLACE, lambda s, uri, start=None, stop=None, config=None:
          session_truncate_replace(orig_session_truncate, s, uri, start, stop, config))

        orig_session_verify = self.Session['verify']
        self.Session['verify'] = (wthooks.HOOK_REPLACE, lambda s, uri, config=None:
          session_verify_replace(orig_session_verify, s, uri, config))

        orig_wiredtiger_open = self.wiredtiger['wiredtiger_open']
        self.wiredtiger['wiredtiger_open'] = (wthooks.HOOK_REPLACE, lambda homedir, config:
                                              wiredtiger_open_replace(orig_wiredtiger_open, homedir, config))

# Strip matching parens, which act as a quoting mechanism.
def strip_matching_parens(s):
    if len(s) >= 2:
        if s[0] == '(' and s[-1] == ')':
            s = s[1:-1]
    return s

def config_split(config):
    pos = config.index('=')
    if pos >= 0:
        left = config[:pos]
        right = config[pos+1:]
    else:
        left = config
        right = ''
    return [left, strip_matching_parens(right)]

# Override some platform APIs for this hook.
class DisaggPlatformAPI(wthooks.WiredTigerHookPlatformAPI):
    def __init__(self, arg=None):
        params = []

        # We allow multiple parameters via
        #  run.py --hook \
        #    'disagg=(param1=value1,param2=value2)'
        #
        # and that should appear as two parameters to the disagg hook.
        if arg:
            arg = strip_matching_parens(arg)
            # Note: this regular expression does not handle arbitrary nesting of parens
            config_list = re.split(r",(?=(?:[^(]*[(][^)]*[)])*[^)]*$)", arg)
            params = [config_split(config) for config in config_list]

        import wttest
        #wttest.WiredTigerTestCase.tty('Disagg hook params={}'.format(params))

        self.disagg_config = ''
        self.disagg_page_log = 'palm'
        self.disagg_role = 'leader'
        self.table_prefix = 'layered'

        for param_key, param_value in params:
            if param_key == 'config':
                self.disagg_config = param_value
            elif param_key == 'page_log':
                self.disagg_page_log = param_value
            elif param_key == 'role':
                self.disagg_role = param_value
            elif param_key == 'table_prefix':
                self.table_prefix = param_value
            else:
                raise Exception('hook_disagg: unknown parameter {}'.format(param_key))

    def setUp(self, testcase):
        # Keep a set of table uris on the test case that we have
        # decided to make into layered tables.  We make the decision
        # when Session.create is called, but in other APIs, we don't have
        # the same information available. This set would probably be more
        # logical to put on the connection, but when we reopen a connection,
        # we won't get the same Python object, so we would lose the set.
        #
        # We also allow a set of uris that we never want to be layered.
        # Tests can add to this list.
        #
        # TODO: When there are multiple connections on multiple home directories,
        # this may break down.  If we want a more precise accounting, we can associate the
        # uris with the home directory used.
        testcase.layered_uris = set()
        testcase.non_layered_uris = set()

    def tearDown(self, testcase):
        if len(testcase.layered_uris) > 0:
            testcase.pr(f'>>>>layered tables: {testcase.layered_uris}<<<<<')

    def tableExists(self, name):
        # TODO: for palm will need to rummage in PALM files.
        return False

    def initialFileName(self, uri):
        # TODO: there really isn't an equivalent
        # return 'kv_home/data.mdb'
        return None

    def getDisaggParameters(self):
        result = wthooks.DisaggParameters()
        result.config = self.disagg_config
        result.role = self.disagg_role
        result.page_log = self.disagg_page_log
        result.table_prefix = self.table_prefix
        return result

# Every hook file must have a top level initialize function,
# returning a list of WiredTigerHook objects.
def initialize(arg):
    return [DisaggHookCreator(arg)]
