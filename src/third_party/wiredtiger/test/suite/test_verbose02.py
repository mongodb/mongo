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

from test_verbose01 import test_verbose_base
from wtscenario import make_scenarios
import wiredtiger, wttest

# test_verbose02.py
# Verify basic uses of the verbose configuration API when categories and valid/invalid verbosity
# levels are specified.
class test_verbose02(test_verbose_base):

    format = [
        ('flat', dict(is_json=False)),
        ('json', dict(is_json=True)),
    ]
    scenarios = make_scenarios(format)

    collection_cfg = 'key_format=S,value_format=S'

    # Test use cases passing single verbose categories, ensuring we only produce verbose output for
    # the single category.
    def test_verbose_single(self):
        # Close the initial connection. We will be opening new connections with different verbosity
        # settings throughout this test.
        self.close_conn()

        # Test passing a single verbose category, 'api' along with the verbosity level
        # WT_VERBOSE_DEBUG (1). Ensuring the only verbose output generated is related to the 'api'
        # category.
        with self.expect_verbose(['api:1'], ['WT_VERB_API'], self.is_json) as conn:
            # Perform a set of simple API operations to generate verbose API messages.
            uri = 'table:test_verbose01_api'
            session = conn.open_session()
            session.create(uri, self.collection_cfg)
            c = session.open_cursor(uri)
            c['api'] = 'api'
            c.close()
            session.close()

        # At this time, there is no verbose messages with the category WT_VERB_API and the verbosity
        # level WT_VERBOSE_INFO (0), hence we don't expect any output.
        with self.expect_verbose(['api:0'], ['WT_VERB_API'], self.is_json, False) as conn:
            uri = 'table:test_verbose01_api'
            session = conn.open_session()
            session.create(uri, self.collection_cfg)
            c = session.open_cursor(uri)
            c['api'] = 'api'
            c.close()
            session.close()

        # Test passing another single verbose category, 'compact' with different verbosity levels.
        # Since there are verbose message with the category WT_VERB_COMPACT and the verbosity levels
        # WT_VERBOSE_INFO (0) and WT_VERBOSE_DEBUG (1), we can test them both.
        cfgs = ['compact:0', 'compact:1']
        for cfg in cfgs:
            with self.expect_verbose([cfg], ['WT_VERB_COMPACT'], self.is_json) as conn:
                # Create a simple table to invoke compaction on. We aren't doing anything
                # interesting with the table, we want to simply invoke a compaction pass to generate
                # verbose messages.
                uri = 'table:test_verbose01_compact'
                session = conn.open_session()
                session.create(uri, self.collection_cfg)
                session.compact(uri)
                session.close()

    # Test use cases passing multiple verbose categories, ensuring we only produce verbose output
    # for specified categories.
    def test_verbose_multiple(self):
        self.close_conn()
        # Test passing multiple verbose categories, being 'api' & 'version' with different dedicated
        # verbosity levels to each category. Ensuring the only verbose output generated is related
        # to those two categories.
        cfgs = ['api:1,version', 'api,version:1', 'api:1,version:1']
        for cfg in cfgs:
            with self.expect_verbose([cfg], ['WT_VERB_API', 'WT_VERB_VERSION'], self.is_json) as conn:
                # Perform a set of simple API operations (table creations and cursor operations) to
                # generate verbose API messages. Beyond opening the connection resource, we
                # shouldn't need to do anything special for the version category.
                uri = 'table:test_verbose01_multiple'
                session = conn.open_session()
                session.create(uri, self.collection_cfg)
                c = session.open_cursor(uri)
                c['multiple'] = 'multiple'
                c.close()

    # Test use cases passing invalid verbosity levels, ensuring the appropriate error message is
    # raised.
    def test_verbose_level_invalid(self):
        self.close_conn()
        # Any negative value is invalid.
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
                lambda:self.wiredtiger_open(self.home, 'verbose=[api:-1]'),
                '/Failed to parse verbose option \'api\'/')
        # Any value greater than WT_VERBOSE_DEBUG (1) is invalid.
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
                lambda:self.wiredtiger_open(self.home, 'verbose=[api:2]'),
                '/Failed to parse verbose option \'api\'/')

if __name__ == '__main__':
    wttest.run()
