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

import time, wiredtiger, wttest, unittest
from wiredtiger import stat

# test_search_near01.py
# Test various prefix search near scenarios.
class test_search_near01(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all)'
    session_config = 'isolation=snapshot'

    def get_stat(self, stat, local_session = None):
        if (local_session != None):
            stat_cursor = local_session.open_cursor('statistics:')
        else:
            stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def unique_insert(self, cursor, prefix, id, keys):
        key = prefix +  ',' +  str(id)
        keys.append(key)
        cursor.set_key(prefix)
        cursor.set_value(prefix)
        self.assertEqual(cursor.insert(), 0)
        cursor.set_key(prefix)
        self.assertEqual(cursor.remove(), 0)
        cursor.set_key(prefix)
        cursor.search_near()
        cursor.set_key(key)
        cursor.set_value(key)
        self.assertEqual(cursor.insert(), 0)

    def test_base_scenario(self):
        uri = 'table:test_base_scenario'
        self.session.create(uri, 'key_format=u,value_format=u')
        cursor = self.session.open_cursor(uri)
        session2 = self.conn.open_session()
        cursor3 = self.session.open_cursor(uri, None, "debug=(release_evict=true)")

        # Basic character array.
        l = "abcdefghijklmnopqrstuvwxyz"

        # Start our older reader.
        session2.begin_transaction()

        key_count = 26*26*26
        # Insert keys aaa -> zzz.
        self.session.begin_transaction()
        for i in range (0, 26):
            for j in range (0, 26):
                for k in range (0, 26):
                    cursor[l[i] + l[j] + l[k]] = l[i] + l[j] + l[k]
        self.session.commit_transaction()

        # Evict the whole range.
        for i in range (0, 26):
            for j in range(0, 26):
                cursor3.set_key(l[i] + l[j] + 'a')
                cursor3.search()
                cursor3.reset()

        # Search near for the "aa" part of the range.
        cursor2 = session2.open_cursor(uri)
        cursor2.set_key('aa')
        cursor2.search_near()

        skip_count = self.get_stat(stat.conn.cursor_next_skip_lt_100)
        # This should be equal to roughly key_count * 2 as we're going to traverse the whole
        # range forward, and then the whole range backwards.
        self.assertGreater(skip_count, key_count * 2)

        cursor2.reconfigure("prefix_search=true")
        cursor2.set_key('aa')
        self.assertEqual(cursor2.search_near(), wiredtiger.WT_NOTFOUND)

        prefix_skip_count = self.get_stat(stat.conn.cursor_next_skip_lt_100)
        # We should've skipped ~26 here as we're only looking at the "aa" range.
        self.assertGreaterEqual(prefix_skip_count - skip_count, 26)
        skip_count = prefix_skip_count

        # The prefix code will have come into play at once as we walked to "aba". The prev
        # traversal will go off the end of the file and as such we don't expect it to increment
        # this statistic again.
        self.assertEqual(self.get_stat(stat.conn.cursor_search_near_prefix_fast_paths), 1)

        # Search for a key not at the start.
        cursor2.set_key('bb')
        self.assertEqual(cursor2.search_near(), wiredtiger.WT_NOTFOUND)

        # Assert it to have only incremented the skipped statistic ~26 times.
        prefix_skip_count = self.get_stat(stat.conn.cursor_next_skip_lt_100)
        self.assertGreaterEqual(prefix_skip_count - skip_count, 26)
        skip_count = prefix_skip_count

        # Here we should have hit the prefix fast path code twice, as we have called prefix
        # search near twice, both of which should have early exited when going forwards.
        self.assertEqual(self.get_stat(stat.conn.cursor_search_near_prefix_fast_paths), 2)

        cursor2.close()
        cursor2 = session2.open_cursor(uri)
        cursor2.set_key('bb')
        cursor2.search_near()
        # Assert that we've incremented the stat key_count times, as we closed the cursor and
        # reopened it.
        #
        # This validates cursor caching logic, as if we don't clear the flag correctly this will
        # fail.
        #
        # It should be closer to key_count * 2 but this an approximation.
        prefix_skip_count = self.get_stat(stat.conn.cursor_next_skip_lt_100)
        self.assertGreaterEqual(prefix_skip_count - skip_count, key_count)

    # This test aims to simulate a unique index insertion.
    def test_unique_index_case(self):
        uri = 'table:test_unique_index_case'
        self.session.create(uri, 'key_format=u,value_format=u')
        cursor = self.session.open_cursor(uri)
        session2 = self.conn.open_session()
        cursor3 = self.session.open_cursor(uri, None, "debug=(release_evict=true)")
        l = "abcdefghijklmnopqrstuvwxyz"

        # A unique index has the following insertion method:
        # 1. Insert the prefix
        # 2. Remove the prefix
        # 3. Search near for the prefix
        # 4. Insert the full value
        # All of these operations are wrapped in the same txn, this test attempts to test scenarios
        # that could arise from this insertion method.

        # A unique index key has the format (prefix, _id), we'll insert keys that look similar.

        # Start our old reader txn.
        session2.begin_transaction()

        key_count = 26*26
        id = 0
        cc_id = 0
        keys = []

        # Insert keys aa,1 -> zz,N
        for i in range (0, 26):
            for j in range (0, 26):
                # Skip inserting 'c'.
                if (i == 2 and j == 2):
                    cc_id = id
                    id = id + 1
                    continue
                self.session.begin_transaction()
                prefix = l[i] + l[j]
                self.unique_insert(cursor, prefix, id, keys)
                id = id + 1
                self.session.commit_transaction()

        # Evict the whole range.
        for i in keys:
            cursor3.set_key(i)
            cursor3.search()
            cursor3.reset()

        # Using our older reader attempt to find a value.
        # Search near for the "cc" prefix.
        cursor2 = session2.open_cursor(uri)
        cursor2.set_key('cc')
        cursor2.search_near()

        skip_count = self.get_stat(stat.conn.cursor_next_skip_lt_100)
        # This should be slightly greater than key_count as we're going to traverse most of the
        # range forwards.
        self.assertGreater(skip_count, key_count)
        self.assertEqual(self.get_stat(stat.conn.cursor_search_near_prefix_fast_paths), 0)

        cursor2.reconfigure("prefix_search=true")
        cursor2.set_key('cc')
        self.assertEqual(cursor2.search_near(), wiredtiger.WT_NOTFOUND)
        self.assertEqual(self.get_stat(stat.conn.cursor_search_near_prefix_fast_paths), 1)

        # This still isn't visible to our older reader and as such we expect this statistic to
        # increment again.
        self.unique_insert(cursor2, 'cc', cc_id, keys)
        self.assertEqual(self.get_stat(stat.conn.cursor_search_near_prefix_fast_paths), 2)

    # In order for prefix key fast pathing to work we rely on some guarantees provided by row
    # search. Test some of the guarantees.
    def test_row_search(self):
        uri = 'table:test_row_search'
        self.session.create(uri, 'key_format=u,value_format=u')
        cursor = self.session.open_cursor(uri)
        expect_count = self.get_stat(stat.conn.cursor_next_skip_lt_100)
        session2 = self.conn.open_session()
        l = "abcdefghijklmnopqrstuvwxyz"
        # Insert keys a -> z, except c
        self.session.begin_transaction()
        for i in range (0, 26):
            if (i == 2):
                continue
            cursor[l[i]] = l[i]
        self.session.commit_transaction()
        # Start our older reader transaction.
        session2.begin_transaction()
        # Insert a few keys in the 'c' range
        self.session.begin_transaction()
        cursor['c'] = 'c'
        cursor['cc'] = 'cc'
        cursor['ccc'] = 'ccc'
        self.session.commit_transaction()
        # Search_near for 'c' and assert we skip 3 entries. Internally the row search is landing on
        # 'c'.
        cursor2 = session2.open_cursor(uri)
        cursor2.set_key('c')
        cursor2.search_near()

        expect_count += 1
        skip_count = self.get_stat(stat.conn.cursor_next_skip_lt_100)
        self.assertEqual(skip_count, expect_count)
        session2.commit_transaction()

        # Perform an insertion and removal of a key next to another key, then search for the
        # removed key.
        self.session.begin_transaction()
        cursor.set_key('dd')
        cursor.set_value('dd')
        cursor.insert()
        cursor.set_key('dd')
        cursor.remove()
        cursor.set_key('ddd')
        cursor.set_value('ddd')
        cursor.insert()
        cursor.set_key('dd')
        cursor.search_near()
        self.session.commit_transaction()
        expect_count += 1
        skip_count = self.get_stat(stat.conn.cursor_next_skip_lt_100)
        self.assertEqual(skip_count, expect_count)

    # Test a basic prepared scenario.
    def test_prepared(self):
        uri = 'table:test_base_scenario'
        self.session.create(uri, 'key_format=u,value_format=u')
        cursor = self.session.open_cursor(uri)
        session2 = self.conn.open_session()
        cursor3 = session2.open_cursor(uri, None, "debug=(release_evict=true)")
        # Insert an update without timestamp
        l = "abcdefghijklmnopqrstuvwxyz"
        session2.begin_transaction()

        key_count = 26*26

        # Insert 'cc'
        self.session.begin_transaction()
        cursor['cc'] = 'cc'
        self.session.commit_transaction()

        # Prepare keys aa -> zz
        self.session.begin_transaction()
        for i in range (0, 26):
            if (i == 2):
                continue
            for j in range (0, 26):
                cursor[l[i] + l[j]] = l[i] + l[j]

        self.session.prepare_transaction('prepare_timestamp=2')

        # Evict the whole range.
        for i in range (0, 26):
            for j in range(0, 26):
                cursor3.set_key(l[i] + l[j])
                cursor3.search()
                cursor3.reset()

        # Search near for the "aa" part of the range.
        cursor2 = session2.open_cursor(uri)
        cursor2.set_key('c')
        self.assertEqual(cursor2.search_near(), wiredtiger.WT_NOTFOUND)

        skip_count = self.get_stat(stat.conn.cursor_next_skip_lt_100, session2)
        # This should be equal to roughly key_count as we're going to traverse the whole
        # range forwards.
        self.assertGreater(skip_count, key_count)

        cursor2.reconfigure("prefix_search=true")
        cursor2.set_key('c')
        self.assertEqual(cursor2.search_near(), wiredtiger.WT_NOTFOUND)

        prefix_skip_count = self.get_stat(stat.conn.cursor_next_skip_lt_100, session2)
        # We expect to traverse one entry and have a buffer to account for anomalies.
        self.assertEqual(prefix_skip_count - skip_count, 2)
        skip_count = prefix_skip_count

        # We early exit here as "cc" is not the last key. 
        self.assertEqual(self.get_stat(stat.conn.cursor_search_near_prefix_fast_paths, session2), 1)

        session2.rollback_transaction()
        session2.begin_transaction('ignore_prepare=true')
        cursor4 = session2.open_cursor(uri)
        cursor4.reconfigure("prefix_search=true")
        cursor4.set_key('c')
        self.assertEqual(cursor4.search_near(), 1)
        prefix_skip_count = self.get_stat(stat.conn.cursor_next_skip_lt_100, session2)
        # We expect to traverse one entry and have a buffer to account for anomalies.
        self.assertEqual(prefix_skip_count - skip_count, 2)
        skip_count = prefix_skip_count

        cursor4.reconfigure("prefix_search=false")
        cursor4.set_key('c')
        ret = cursor4.search_near()
        self.assertTrue(ret == -1 or ret == 1)
        self.assertEqual(self.get_stat(stat.conn.cursor_next_skip_lt_100, session2) - skip_count, 2)
