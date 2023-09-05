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

import time
import wttest
import wiredtiger
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wiredtiger import stat

# test_evict01.py
#
# Test that deleted pages are added to eviction as part of the tree walk.

class test_evict01(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=500MB,statistics=(all)'

    format_values = [
    # FLCS doesn't support skipping pages based on aggregated time window.
    #    ('column-fix', dict(key_format='r', value_format='8t',
    #        extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('column', dict(key_format='r', value_format='S',
            extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('string_row', dict(key_format='S', value_format='S',
             extraconfig=',allocation_size=512,leaf_page_max=512')),
    ]

    timestamp_values = [
        ('no_timestamp', dict(timestamp=False)),
        ('timestamp', dict(timestamp=True)),
    ]

    scenarios = make_scenarios(format_values, timestamp_values)

    def check(self, session, ds, nrows):
        cursor = session.open_cursor(ds.uri)
        count = 0
        for k, v in cursor:
            count += 1
        self.assertEqual(count, nrows)
        cursor.close()

    def test_evict(self):
        uri = 'table:evict01'
        nrows = 10000

        # Create a table.
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()

        value_a = "aaaaa" * 100

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Write some initial data.
        cursor = self.session.open_cursor(ds.uri, None, None)
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor[ds.key(i)] = value_a + str(i)
            if self.timestamp:
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
            else:
                self.session.commit_transaction()
        cursor.close()

        # Mark it stable.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # Reopen the connection (which checkpoints it) so it's all on disk and not in memory.
        self.reopen_conn()

        # Create a reader transaction that will not be able to see what happens next.
        # We don't need to do anything with this; it just needs to exist.
        session2 = self.conn.open_session()
        session2.begin_transaction()

        # Now remove most of data leaving the first and last range of keys.
        cursor = self.session.open_cursor(ds.uri, None, None)
        for i in range(101, nrows - 99):
            self.session.begin_transaction()
            cursor.set_key(ds.key(i))
            self.assertEqual(cursor.remove(), 0)
            if self.timestamp:
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
            else:
                self.session.commit_transaction()

        # Checkpoint.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.session.checkpoint()

        # Get the existing cache eviction force delete statistic value.
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        prev_cache_eviction_force_delete = stat_cursor[stat.conn.cache_eviction_force_delete][2]
        prev_evict = stat_cursor[stat.conn.cache_eviction_dirty][2]
        stat_cursor.close()
        self.assertEqual(prev_cache_eviction_force_delete, 0)

        # Now read the data.
        self.check(self.session, ds, 200)

        # Get the new cache eviction force delete statistic value.
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        cache_eviction_force_delete = stat_cursor[stat.conn.cache_eviction_force_delete][2]
        stat_cursor.close()

        self.assertGreater(cache_eviction_force_delete, prev_cache_eviction_force_delete)

        # Wait for the eviction to happen.
        evict = 0
        wait_count = 0
        while wait_count < 30 and evict <= prev_evict:
            time.sleep(1)
            stat_cursor = self.session.open_cursor('statistics:', None, None)
            evict = stat_cursor[stat.conn.cache_eviction_dirty][2]
            stat_cursor.close()
            wait_count+=1

        # Scan the table again, this time the deleted pages must be skipped at page level.
        self.check(self.session, ds, 200)

        # Get the tree walk delete page skip statistic value.
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        cursor_tree_walk_del_page_skip = stat_cursor[stat.conn.cursor_tree_walk_del_page_skip][2]
        stat_cursor.close()

        # Verify the skip count only when we detected a successful eviction.
        if wait_count < 30:
            self.assertGreater(cursor_tree_walk_del_page_skip, 0)
        cursor.close()

        # All data are visible to the long-running transaction.
        self.check(session2, ds, nrows)

        # Close the long running transaction.
        session2.rollback_transaction()
        session2.close()

if __name__ == '__main__':
    wttest.run()
