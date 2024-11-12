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

import wiredtiger, wttest
# test_bug34.py
# This tests for the scenario discovered in WT-12602.
# Before WT-12602, it was possible that evicting a page when checkpoint is happening parallel would
# lead to an incorrect EBUSY error. The page that was getting evicted required a modify to be
# written to history store and the oldest update in the history store as a globally visible tombstone.
# For this condition to occur, we must have the following:
# - The history page must be reconciled and written to disk
# - One transaction needs to have an modify and update on the record.
# - There needs to be a globally visible tombstone that is older than the modify
# and update on the record.
class test_bug34(wttest.WiredTigerTestCase):
    # Configure debug behavior on evict, where eviction threads would evict as if checkpoint was in
    # parallel.
    conn_config = 'cache_size=500MB,statistics=(all),debug_mode=(eviction_checkpoint_ts_ordering=true)'
    nrows = 100

    def evict_cursor(self, uri, nrows):
        s = self.conn.open_session()
        s.begin_transaction()
        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = s.open_cursor(uri, None, "debug=(release_evict)")
        for i in range(1, nrows + 1):
            evict_cursor.set_key(str(i))
            evict_cursor.search()
            evict_cursor.reset()
        s.rollback_transaction()
        evict_cursor.close()

    def test_non_ts(self):
        uri = 'table:test_bug034'
        create_params = 'key_format=S,value_format=S'
        self.session.create(uri, create_params)
        value1 = 'a' * 500
        value2 = 'b' * 500

        # Populate data in the data store table.
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, self.nrows):
            cursor[str(i)] = value1
        self.session.commit_transaction()

        # Write the data to disk.
        self.session.checkpoint()

        # Add in a tombstone.
        self.session.begin_transaction()
        for i in range(1, self.nrows):
            cursor.set_key(str(i))
            self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction()

        # Make sure that we don't move the oldest ID forward, so create a second long running
        # transaction.
        session2 = self.conn.open_session()
        session2.begin_transaction()

        # Add in a update and a modify.
        self.session.begin_transaction()
        for i in range(1, self.nrows):
            cursor[str(i)] = value2

            cursor.set_key(str(i))
            mods = [wiredtiger.Modify("b", 0, 1)]
            self.assertEqual(cursor.modify(mods), 0)
        self.session.commit_transaction()

        # Apply update again to make sure that the update, modify and tombstome all go
        # to the HS.
        self.session.begin_transaction()
        for i in range(1, self.nrows):
            cursor[str(i)] = value1
        self.session.commit_transaction()

        # Reconcile all data onto the disk.
        self.session.checkpoint()

        # Perform dirty eviction to trigger reconciliation.
        self.session.begin_transaction()
        for i in range(1, self.nrows):
            cursor[str(i)] = value1
        self.session.commit_transaction()
        self.evict_cursor(uri, self.nrows)

        session2.commit_transaction()

    def test_ts(self):
        uri = 'table:test_bug034'
        create_params = 'key_format=S,value_format=S'
        self.session.create(uri, create_params)
        value1 = 'a' * 500
        value2 = 'b' * 500

        # Populate data in the data store table.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, self.nrows):
            cursor[str(i)] = value1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(5))

        # Write the data to disk.
        self.session.checkpoint()

        # Add in a globally visible tombstone.
        self.session.begin_transaction('no_timestamp=true')
        for i in range(1, self.nrows):
            cursor.set_key(str(i))
            self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction()

        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(7))

        # Add in a update and a modify.
        self.session.begin_transaction()
        for i in range(1, self.nrows):
            cursor[str(i)] = value2

            cursor.set_key(str(i))
            mods = [wiredtiger.Modify("b", 0, 1)]
            self.assertEqual(cursor.modify(mods), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(8))

        # Apply update again to make sure that the update, modify and tombstome all go
        # to the HS.
        self.session.begin_transaction()
        for i in range(1, self.nrows):
            cursor[str(i)] = value1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        # Reconcile all data onto the disk.
        self.session.checkpoint()

        # Perform dirty eviction to trigger reconciliation.
        self.session.begin_transaction()
        for i in range(1, self.nrows):
            cursor[str(i)] = value1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(11))
        self.evict_cursor(uri, self.nrows)

