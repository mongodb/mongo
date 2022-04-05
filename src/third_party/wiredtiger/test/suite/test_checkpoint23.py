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

# test_checkpoint23.py
#
# Test that obsolete pages with overflow items are gradually cleaned up by
# checkpoints even if they aren't in memory.

import wttest
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

class test_checkpoint23(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB,statistics=(all)'

    # There is no point running this on FLCS because FLCS does not have overflow items.
    # For now there is no point running it on VLCS (and it'll fail) because the checkpoint
    # cleanup code does not handle VLCS pages. But the VLCS case should be enabled if that
    # changes, e.g. as part of VLCS fast-delete support.
    format_values = [
        ('row_string', dict(key_format='S', value_format='S')),
        #('column', dict(key_format='r', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def get_tree_stat(self, ds, stat):
        stat_cursor = self.session.open_cursor('statistics:' + ds.uri, None, 'statistics=(all)')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def large_updates(self, ds, nrows, primary_value, secondary_value, commit_ts):
        session = self.session
        cursor = session.open_cursor(ds.uri)
        for i in range(1, nrows + 1):
            session.begin_transaction()
            cursor[ds.key(i)] = secondary_value if i % 10 == 1 else primary_value
            session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

    def large_removes(self, ds, nrows, commit_ts):
        session = self.session
        cursor = session.open_cursor(ds.uri)
        for i in range(1, nrows + 1):
            session.begin_transaction()
            cursor.set_key(ds.key(i))
            self.assertEqual(cursor.remove(), 0)
            session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

    def test_checkpoint23(self):
        uri = 'table:checkpoint23'
        nrows = 10000

        # Create a table.
        ds = SimpleDataSet(self, uri, nrows,
            key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        # Pin oldest and stable timestamp to 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Use a seconary value large enough to be an overflow item.
        value_a = "aaaaa" * 100
        value_b = "bbbbb" * 10000

        # Write a bunch of data at time 10.
        self.large_updates(ds, nrows, value_a, value_b, 10)

        # Delete it all at time 20.
        self.large_removes(ds, nrows, 20)

        # Mark it stable at 30 and checkpoint it.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        self.session.checkpoint()

        # Shut down and restart so nothing's in memory.
        self.reopen_conn()

        # Check how many overflow items we have.
        initial_overflow_items = self.get_tree_stat(ds, stat.dsrc.btree_overflow)

        # Shut down and restart again so nothing's in memory.
        # (Counting overflow items reads pages in.)
        self.reopen_conn()

        # Move oldest up so all the data is obsolete.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(30))

        # Do a checkpoint. Name the checkpoint to force things through.
        self.session.checkpoint('name=pointy')

        # Do another checkpoint to make sure the updated pages get reconciled.
        # Otherwise the overflow items might not actually be removed.
        self.session.checkpoint()

        # Count the overflow items; should have dropped.
        updated_overflow_items = self.get_tree_stat(ds, stat.dsrc.btree_overflow)
        removed = initial_overflow_items - updated_overflow_items
        self.assertGreater(removed, 0)

        # Check how many on-disk pages we read in to do this. Should be > 0.
        pages_read = self.get_stat(stat.conn.cc_pages_read)
        self.assertGreater(pages_read, 0)

if __name__ == '__main__':
    wttest.run()
