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

import wiredtiger, wttest, unittest
from wiredtiger import stat
from wtbound import set_prefix_bound

# test_cursor_bound11.py
# Test various cursor bound prefix search scenarios.
# This test has been migrated to use the bounded cursor logic.
class test_cursor_bound11(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all)'

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

        skip_count = self.get_stat(stat.conn.cursor_next_skip_total)
        # This should be equal to key_count - 1 as we're going to traverse the whole
        # range forward from "aa".
        self.assertEqual(skip_count, key_count - 1)

        set_prefix_bound(self, cursor2, 'aa')
        cursor2.set_key('aa')
        self.assertEqual(cursor2.search_near(), wiredtiger.WT_NOTFOUND)

        bound_skip_count = self.get_stat(stat.conn.cursor_next_skip_total)
        # We should've skipped 26 - 1 here as we're only looking at the "aa" range starting from "aa".
        self.assertGreaterEqual(bound_skip_count - skip_count, 25)
        skip_count = bound_skip_count

        # The bounded cursor logic will have come into play at once as we walked to "aba". The prev
        # traversal will go off the end of the file and as such we don't expect it to increment
        # this statistic again.
        self.assertEqual(self.get_stat(stat.conn.cursor_bounds_next_early_exit), 1)

        # Search for a key not at the start.
        set_prefix_bound(self, cursor2, 'bb')
        cursor2.set_key('bb')
        self.assertEqual(cursor2.search_near(), wiredtiger.WT_NOTFOUND)

        # Assert it to have only incremented the skipped statistic ~26 times.
        bound_skip_count = self.get_stat(stat.conn.cursor_next_skip_total)
        self.assertGreaterEqual(bound_skip_count - skip_count, 26)
        skip_count = bound_skip_count

        # Here we should hit the bounded cursor fast path an additional time.
        self.assertEqual(self.get_stat(stat.conn.cursor_bounds_next_early_exit), 2)

        cursor2.close()
        cursor2 = session2.open_cursor(uri)
        cursor2.set_key('aa')
        cursor2.search_near()
        # Assert that we've incremented the stat key_count times, as we closed the cursor and
        # reopened it.
        #
        # This validates cursor caching logic, as if we don't clear the flag correctly this will
        # fail.
        bound_skip_count = self.get_stat(stat.conn.cursor_next_skip_total)
        self.assertGreaterEqual(bound_skip_count - skip_count, key_count - 1)

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

        skip_count = self.get_stat(stat.conn.cursor_next_skip_total)
        # This should be slightly greater than key_count as we're going to traverse most of the
        # range forwards.
        self.assertGreater(skip_count, key_count)
        self.assertEqual(self.get_stat(stat.conn.cursor_bounds_next_early_exit), 0)

        set_prefix_bound(self, cursor2, 'cc')
        cursor2.set_key('cc')
        self.assertEqual(cursor2.search_near(), wiredtiger.WT_NOTFOUND)
        self.assertEqual(self.get_stat(stat.conn.cursor_bounds_next_early_exit), 1)

        # This still isn't visible to our older reader and as such we expect this statistic to
        # increment again.
        self.unique_insert(cursor2, 'cc', cc_id, keys)
        self.assertEqual(self.get_stat(stat.conn.cursor_bounds_next_early_exit), 2)

    # In order for cursor bound fast pathing to work we rely on some guarantees provided by row
    # search. Test some of the guarantees.
    def test_row_search(self):
        uri = 'table:test_row_search'
        self.session.create(uri, 'key_format=u,value_format=u')
        cursor = self.session.open_cursor(uri)
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
        # Search_near for 'c' and assert we skip 2 entries.
        cursor2 = session2.open_cursor(uri)
        cursor2.set_key('c')
        cursor2.search_near()
        skip_count = self.get_stat(stat.conn.cursor_next_skip_total)
        # We are positioning on 'c' and skipping over 'cc' and 'ccc'.
        self.assertEqual(skip_count, 2)
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
        # We aren't skipping any entries as we position on 'dd' and walk once to 'ddd'.
        new_skip_count = self.get_stat(stat.conn.cursor_next_skip_total)
        self.assertEqual(new_skip_count - skip_count, 0)

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
        key_count = 0

        # Insert 'cc'
        self.session.begin_transaction()
        cursor['cc'] = 'cc'
        self.session.commit_transaction()

        # Prepare keys aa -> zz but not 'cX'
        self.session.begin_transaction()
        for i in range (0, 26):
            if (i == 2):
                continue
            for j in range (0, 26):
                cursor[l[i] + l[j]] = l[i] + l[j]
                key_count += 1
        cursor.reset()
        self.session.prepare_transaction('prepare_timestamp=2')

        # Evict the whole range.
        for i in range (0, 26):
            for j in range(0, 26):
                cursor3.set_key(l[i] + l[j])
                cursor3.search()
                cursor3.reset()

        # Search near for the 'c' key. We can't see this as our txn started before 'cc' was inserted.
        cursor2 = session2.open_cursor(uri)
        cursor2.set_key('c')
        self.assertEqual(cursor2.search_near(), wiredtiger.WT_NOTFOUND)

        skip_count = self.get_stat(stat.conn.cursor_next_skip_total, session2)
        # This should be equal to roughly key_count as we're going to traverse the whole
        # range forwards. Not including 'a' and 'b'.
        self.assertGreaterEqual(skip_count, key_count - 2*26)

        set_prefix_bound(self, cursor2, 'c')
        cursor2.set_key('c')
        self.assertEqual(cursor2.search_near(), wiredtiger.WT_NOTFOUND)

        bound_skip_count = self.get_stat(stat.conn.cursor_next_skip_total, session2)
        # We expect to traverse one entry, 'cc'.
        self.assertEqual(bound_skip_count - skip_count, 1)
        skip_count = bound_skip_count

        # We early exit here as "cc" is not the last key. 
        self.assertEqual(self.get_stat(stat.conn.cursor_bounds_next_early_exit, session2), 1)

        session2.rollback_transaction()
        session2.begin_transaction('ignore_prepare=true')
        cursor4 = session2.open_cursor(uri)
        set_prefix_bound(self, cursor4, 'c')
        cursor4.set_key('c')
        self.assertEqual(cursor4.search_near(), 1)
        bound_skip_count = self.get_stat(stat.conn.cursor_next_skip_total, session2)
        # We expect to not skip any entries and return 'cc'
        self.assertEqual(bound_skip_count - skip_count, 0)
        self.assertEqual(cursor4.get_key(), b'cc')
        skip_count = bound_skip_count

        cursor4.bound("action=clear")
        cursor4.set_key('c')
        ret = cursor4.search_near()
        self.assertTrue(ret == -1 or ret == 1)
        # We expect to not skip any entries and return 'cc'
        self.assertEqual(self.get_stat(stat.conn.cursor_next_skip_total, session2) - skip_count, 0)
