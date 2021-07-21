#!/usr/bin/env python
#
# Public Domain 2014-2020 MongoDB, Inc.
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
from wtscenario import make_scenarios
from wiredtiger import stat, WT_NOTFOUND

def timestamp_str(t):
    return '%x' % t

# test_cursor17.py
# Test the cursor traversal optimization for delete heavy workloads. This optimization enables
# cursor traversal mechanism to skip pages where all records on the page are deleted with a
# tombstone visible to the current transaction.
class test_cursor17(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB,statistics=(all)'
    session_config = 'isolation=snapshot'

    def get_stat(self, stat, uri):
        stat_string = 'statistics:'
        if (uri):
            stat_string += uri
        stat_cursor = self.session.open_cursor(stat_string)
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def test_cursor_skip_pages(self):
        uri = 'table:test_cursor17'
        create_params = 'key_format=i,value_format=S'
        self.session.create(uri, create_params)

        value1 = 'a' * 500
        value2 = 'b' * 500
        total_keys = 40000

        # Keep the oldest and the stable timestamp pinned.
        self.conn.set_timestamp('oldest_timestamp=' + timestamp_str(1))
        self.conn.set_timestamp('stable_timestamp=' + timestamp_str(2))
        cursor = self.session.open_cursor(uri)

        commit_timestamp = 3

        # Insert predefined number of key-value pairs.
        for key in range(total_keys):
            self.session.begin_transaction()
            cursor[key] = value1
            self.session.commit_transaction('commit_timestamp=' + timestamp_str(commit_timestamp))
            commit_timestamp += 1

        # Delete everything on the table except for the first and the last KV pair.
        for key in range(1, total_keys - 1):
            self.session.begin_transaction()
            cursor.set_key(key)
            self.assertEqual(cursor.remove(),0)
            self.session.commit_transaction('commit_timestamp=' + timestamp_str(commit_timestamp))
            commit_timestamp += 1

        # Take a checkpoint to reconcile the pages.
        self.session.checkpoint()

        self.session.begin_transaction('read_timestamp=' + timestamp_str(commit_timestamp))
        # Position the cursor on the first record.
        cursor.set_key(0)
        self.assertEqual(cursor.search(), 0)
        # This should move the cursor to the last record.
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), total_keys - 1)

        # Check if we skipped any pages while moving the cursor.
        #
        # We calculate the number of pages we expect to skip based on the total number of leaf pages
        # reported in the WT stats. We subtract 2 from the count of leaf pages in the table and test
        # that we atleast skipped 80% of the expected number of pages.

        leaf_pages_in_table = self.get_stat(stat.dsrc.btree_row_leaf, uri)
        expected_pages_skipped = ((leaf_pages_in_table - 2) * 8) // 10
        skipped_pages = self.get_stat(stat.conn.cursor_next_skip_page_count, None)
        self.assertGreater(skipped_pages, expected_pages_skipped)
        self.session.rollback_transaction()

        # Update a key in the middle of the table.
        self.session.begin_transaction()
        cursor[total_keys // 2] = value2
        self.session.commit_transaction('commit_timestamp=' + timestamp_str(commit_timestamp))
        commit_timestamp += 1

        # Make sure we can reach a the record we updated in the middle of the table.
        self.session.begin_transaction('read_timestamp=' + timestamp_str(commit_timestamp))
        # Position the cursor on the first record.
        cursor.set_key(0)
        self.assertEqual(cursor.search(), 0)
        # This should move the cursor to the record we updated in the middle.
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), total_keys // 2)
