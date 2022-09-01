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
from wtscenario import make_scenarios
from wtbound import bound_base
from wiredtiger import stat

# test_cursor_bound08.py
# Test that the statistics added for cursor bound API are appropiately incremented for all different cursor
# operations and edge cases.
class test_cursor_bound08(bound_base):
    conn_config = 'statistics=(all)'
    file_name = 'test_cursor_bound08'
    value_format = 'S'
    lower_inclusive = True
    upper_inclusive = True

    types = [
        ('file', dict(uri='file:', use_colgroup=False)),
        ('table', dict(uri='table:', use_colgroup=False)),
    ]

    key_format_values = [
        ('string', dict(key_format='S')),
        ('var', dict(key_format='r')),
        ('int', dict(key_format='i')),
        ('bytes', dict(key_format='u')),
        ('composite_string', dict(key_format='SSS')),
        ('composite_int_string', dict(key_format='iS')),
        ('composite_complex', dict(key_format='iSru')),
    ]

    evict = [
        ('inclusive-evict', dict(evict=True)),
        ('inclusive-no-evict', dict(evict=False)),
    ]

    scenarios = make_scenarios(types, key_format_values, evict)

    def create_session_and_cursor_timestamp(self):
        nrows = 1000
        uri = self.uri + self.file_name
        create_params = 'value_format=S,key_format={}'.format(self.key_format)
        self.session.create(uri, create_params)

        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(100, 301):
            cursor[self.gen_key(i)] = "value" + str(i)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(100))

        self.session.begin_transaction()
        for i in range(301, 601):
            cursor[self.gen_key(i)] = "value" + str(i)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(200))

        self.session.begin_transaction()
        for i in range(601, nrows + 1):
            cursor[self.gen_key(i)] = "value" + str(i)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(100))

        if (self.evict):
            evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
            for i in range(100, nrows + 1):
                evict_cursor.set_key(self.gen_key(i))
                evict_cursor.search()
                evict_cursor.reset()
            evict_cursor.close()
        return cursor

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def test_bound_basic_stat_scenario(self):
        cursor = self.create_session_and_cursor()

        # Test bound api: Test that early exit stat gets incremented with a upper bound.
        self.set_bounds(cursor, 50, "upper")
        self.cursor_traversal_bound(cursor, None, 50, True)
        self.assertEqual(cursor.bound("action=clear"), 0)
        self.assertEqual(self.get_stat(stat.conn.cursor_bounds_next_early_exit), 1)

        # Test bound api: Test that early exit stat gets incremented with a lower bound.
        self.set_bounds(cursor, 45, "lower")
        self.cursor_traversal_bound(cursor, 45, None, False)
        self.assertEqual(cursor.bound("action=clear"), 0)
        self.assertEqual(self.get_stat(stat.conn.cursor_bounds_prev_early_exit), 1)

        # Test bound api: Test that cursor next unpositioned stat gets incremented with a lower bound.
        self.set_bounds(cursor, 45, "lower")
        self.cursor_traversal_bound(cursor, 45, None, True)
        self.assertEqual(cursor.bound("action=clear"), 0)
        self.assertEqual(self.get_stat(stat.conn.cursor_bounds_next_unpositioned), 1)
    
         # Test bound api: Test that cursor prev unpositioned stat gets incremented with an upper bound.
        self.set_bounds(cursor, 50, "upper")
        self.cursor_traversal_bound(cursor, None, 50, False)
        self.assertEqual(cursor.bound("action=clear"), 0)
        self.assertEqual(self.get_stat(stat.conn.cursor_bounds_prev_unpositioned), 1)

        # Test bound api: Test that both stats get incremented with both bounds set.
        self.set_bounds(cursor, 45, "lower")
        self.set_bounds(cursor, 50, "upper")
        self.cursor_traversal_bound(cursor, 45, 50, True)
        self.assertEqual(cursor.bound("action=clear"), 0)
        self.assertEqual(self.get_stat(stat.conn.cursor_bounds_next_early_exit), 2)
        self.assertEqual(self.get_stat(stat.conn.cursor_bounds_next_unpositioned), 2)

        self.set_bounds(cursor, 45, "lower")
        self.set_bounds(cursor, 50, "upper")
        self.cursor_traversal_bound(cursor, 45, 50, False)
        self.assertEqual(cursor.bound("action=clear"), 0)
        self.assertEqual(self.get_stat(stat.conn.cursor_bounds_prev_early_exit), 2)
        self.assertEqual(self.get_stat(stat.conn.cursor_bounds_prev_unpositioned), 2)

        # Test bound api: Test that cursor bound reset stats get incremented when clearing bounds.
        self.set_bounds(cursor, 45, "lower")
        self.set_bounds(cursor, 50, "upper")
        self.assertEqual(cursor.reset(), 0)
        self.assertEqual(self.get_stat(stat.conn.cursor_bounds_reset), 1)

        # Test bound api: Test that cursor bound search early exit stats get incremented.
        self.set_bounds(cursor, 40, "upper")
        cursor.set_key(self.gen_key(60))
        ret = cursor.search()
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        self.assertEqual(cursor.bound("action=clear"), 0)
        self.assertEqual(self.get_stat(stat.conn.cursor_bounds_search_early_exit), 1)

        self.set_bounds(cursor, 30, "lower")
        cursor.set_key(self.gen_key(20))
        ret = cursor.search()
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        self.assertEqual(cursor.bound("action=clear"), 0)
        self.assertEqual(self.get_stat(stat.conn.cursor_bounds_search_early_exit), 2)

        self.set_bounds(cursor, 30, "lower")
        cursor.set_key(self.gen_key(20))
        self.assertEqual(cursor.search_near(), 1)
        self.assertEqual(cursor.get_key(), self.check_key(30))
        self.assertEqual(cursor.reset(), 0)
        self.assertEqual(self.get_stat(stat.conn.cursor_bounds_search_near_repositioned_cursor), 1)

        # Test bound api: Test that cursor bound search near reposition exit stats get incremented.
        # This can only happen when the search key is out of the bound range.
        self.set_bounds(cursor, 30, "lower")
        cursor.set_key(self.gen_key(20))
        self.assertEqual(cursor.search_near(), 1)
        self.assertEqual(cursor.get_key(), self.check_key(30))
        self.assertEqual(cursor.reset(), 0)
        self.assertEqual(self.get_stat(stat.conn.cursor_bounds_search_near_repositioned_cursor), 2)

        self.set_bounds(cursor, 40, "upper")
        cursor.set_key(self.gen_key(60))
        self.assertEqual(cursor.search_near(), -1)
        self.assertEqual(cursor.get_key(), self.check_key(40))
        self.assertEqual(cursor.reset(), 0)
        self.assertEqual(self.get_stat(stat.conn.cursor_bounds_search_near_repositioned_cursor), 3)

        cursor.close()

    def test_bound_perf_stat_scenario(self):
        cursor = self.create_session_and_cursor_timestamp()

        key_count = 900
        # Make sure to run the test with no records visible.
        self.session.begin_transaction('read_timestamp=' +  self.timestamp_str(50))

        # Test bound api: Test that cursor bound search near traverses less entries perf on upper bounds.
        cursor.set_key(self.gen_key(200))
        self.assertEqual(cursor.search_near(), wiredtiger.WT_NOTFOUND)
        skip_count = self.get_stat(stat.conn.cursor_next_skip_total) + self.get_stat(stat.conn.cursor_prev_skip_total)
        # This should be equal to roughly key_count * 2 as we're going to traverse the whole
        # range forward, and then the whole range backwards.
        self.assertGreater(skip_count, 2 * key_count - 200)

        prev_skip_count = skip_count
        self.set_bounds(cursor, 300, "upper")
        cursor.set_key(self.gen_key(200))
        self.assertEqual(cursor.search_near(), wiredtiger.WT_NOTFOUND)
        skip_count = self.get_stat(stat.conn.cursor_next_skip_total) + self.get_stat(stat.conn.cursor_prev_skip_total)
        self.assertEqual(cursor.bound("action=clear"), 0)
        self.assertGreater(skip_count - prev_skip_count, 50 * 2)

        # Test bound api: Test that cursor bound search near traverses less entries perf on lower bounds.
        prev_skip_count = skip_count
        cursor.set_key(self.gen_key(900))
        self.assertEqual(cursor.search_near(), wiredtiger.WT_NOTFOUND)
        skip_count = self.get_stat(stat.conn.cursor_next_skip_total) + self.get_stat(stat.conn.cursor_prev_skip_total)
        # This should be equal to roughly key_count * 2 as we're going to traverse the whole
        # range forward, and then the whole range backwards.
        self.assertGreater(skip_count - prev_skip_count, 2 * key_count - 900)

        prev_skip_count = skip_count
        self.set_bounds(cursor, 900, "lower")
        cursor.set_key(self.gen_key(900))
        self.assertEqual(cursor.search_near(), wiredtiger.WT_NOTFOUND)
        skip_count = self.get_stat(stat.conn.cursor_next_skip_total) + self.get_stat(stat.conn.cursor_prev_skip_total)
        self.assertEqual(cursor.bound("action=clear"), 0)
        self.assertGreater(skip_count - prev_skip_count, 100)

        # Test bound api: Test that cursor bound search near traverses less entries perf on both bounds.
        prev_skip_count = skip_count
        cursor.set_key(self.gen_key(750))
        self.assertEqual(cursor.search_near(), wiredtiger.WT_NOTFOUND)
        skip_count = self.get_stat(stat.conn.cursor_next_skip_total) + self.get_stat(stat.conn.cursor_prev_skip_total)
        # This should be equal to roughly key_count * 2 as we're going to traverse the whole
        # range forward, and then the whole range backwards.
        self.assertGreater(skip_count - prev_skip_count, 2 * key_count - 750)

        prev_skip_count = skip_count
        self.set_bounds(cursor, 600, "lower")
        self.set_bounds(cursor, 900, "upper")
        cursor.set_key(self.gen_key(750))
        self.assertEqual(cursor.search_near(), wiredtiger.WT_NOTFOUND)
        skip_count = self.get_stat(stat.conn.cursor_next_skip_total) + self.get_stat(stat.conn.cursor_prev_skip_total)
        self.assertEqual(cursor.bound("action=clear"), 0)
        self.assertGreater(skip_count - prev_skip_count, 150 * 2)

        cursor.close()

if __name__ == '__main__':
    wttest.run()
