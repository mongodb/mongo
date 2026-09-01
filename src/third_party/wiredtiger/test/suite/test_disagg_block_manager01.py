#!/usr/bin/env python3
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

import wiredtiger, wttest
from helper_disagg import disagg_test_class

# A table using the disaggregated block manager needs a page log to read or write anything. A
# connection that never configured disaggregated storage has none, in which case the disaggregated
# block manager declines the object and the tree is handed to the local one, leaving a tree flagged
# disaggregated whose blocks live in a local file. Nothing downstream reconciles the two, so the
# combination has to be refused when the handle is opened rather than once the tree is written out.
class test_disagg_block_manager01(wttest.WiredTigerTestCase):
    nrows = 5000
    value = 'a' * 512

    # The refusal is reported on the error stream as well as through the return code.
    no_page_log = 'requires a page log'

    def populate(self, uri):
        cursor = self.session.open_cursor(uri)
        for i in range(self.nrows):
            cursor[str(i)] = self.value
        cursor.close()

    def test_create_rejected(self):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create('table:disagg',
                'key_format=S,value_format=S,block_manager=disagg'),
            '/Invalid argument/')
        self.ignoreStderrPatternIfExists(self.no_page_log)

    def test_connection_survives_rejected_create(self):
        # A table that does not ask for the disaggregated block manager is unaffected, and stays
        # usable across the rejected create.
        plain = 'table:plain'
        self.session.create(plain, 'key_format=S,value_format=S')
        self.populate(plain)

        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.create('table:disagg',
                'key_format=S,value_format=S,block_manager=disagg'))

        # Writing out the trees is what used to abort the process.
        self.session.checkpoint()

        cursor = self.session.open_cursor(plain)
        self.assertEqual(cursor[str(self.nrows - 1)], self.value)
        cursor.close()

        self.ignoreStderrPatternIfExists(self.no_page_log)

        # The rejected create must not leave anything behind that a later open trips over, so the
        # database has to come back cleanly and the table must not be in the metadata.
        self.reopen_conn()

        uris = []
        meta = self.session.open_cursor('metadata:create')
        while meta.next() == 0:
            uris.append(meta.get_key())
        meta.close()

        # The surviving table proves the walk ran; the rejected one must be absent.
        self.assertIn(plain, uris)
        self.assertEqual([u for u in uris if 'disagg' in u], [], 'stale metadata: %s' % uris)

        cursor = self.session.open_cursor(plain)
        self.assertEqual(cursor[str(self.nrows - 1)], self.value)
        cursor.close()

# The same combination reached from the other direction: a table created while a page log was
# configured, opened later by a connection that has none. The configuration outlives the connection
# that accepted it, so the refusal has to happen on every open of the tree, not only on create.
@disagg_test_class
class test_disagg_block_manager01_reopen(wttest.WiredTigerTestCase):
    conn_config = 'disaggregated=(role="leader")'

    no_page_log = test_disagg_block_manager01.no_page_log

    # Set before the reopen, so the connection comes back with no page log to resolve the table's
    # configuration against. Dropping the page log from the connection configuration alone is not
    # enough: the name is recorded in the base configuration and in the table's own metadata, and a
    # reopened connection finds it again through either.
    drop_page_log = False

    def conn_extensions(self, extlist):
        if self.drop_page_log:
            return
        self.add_scenario_config()
        return self.disagg_conn_extensions(extlist)

    def test_reopen_rejected(self):
        uri = 'table:disagg'
        self.session.create(uri, 'key_format=S,value_format=S,block_manager=disagg')
        cursor = self.session.open_cursor(uri)
        cursor['key'] = 'value'
        cursor.close()
        self.session.checkpoint()

        # The connection itself has no reason to fail: only the tables that need the page log do.
        self.drop_page_log = True
        self.reopen_conn(config='create,config_base=false')

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(uri), '/Invalid argument/')
        self.ignoreStderrPatternIfExists(self.no_page_log)
