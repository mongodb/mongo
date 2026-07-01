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

import wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wiredtiger import stat

#
# Test that skipping in-memory reconciled deleted pages as part of the tree walk.
class test_checkpoint32(wttest.WiredTigerTestCase):

    format_values = [
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('string_row', dict(key_format='S', value_format='S', extraconfig='')),
    ]

    ckpt_precision = [
        ('fuzzy', dict(ckpt_config='precise_checkpoint=false')),
        ('precise', dict(ckpt_config='precise_checkpoint=true')),
    ]

    scenarios = make_scenarios(format_values, ckpt_precision)

    def conn_config(self):
        return self.ckpt_config

    def check(self, ds, nrows, value):
        cursor = self.session.open_cursor(ds.uri)
        count = 0
        for k, v in cursor:
            self.assertEqual(v, value)
            count += 1
        self.assertEqual(count, nrows)
        cursor.close()

    def test_checkpoint(self):
        # Avoid checkpoint error with precise checkpoint
        if self.ckpt_config == 'precise_checkpoint=true':
            self.conn.set_timestamp('stable_timestamp=1')

        uri = 'table:checkpoint32'
        nrows = 1000

        # Create a table.
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()

        value_a = "aaaaa" * 100

        # Write some initial data.
        cursor = self.session.open_cursor(ds.uri, None, None)
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor[ds.key(i)] = value_a
            self.session.commit_transaction()

        # Create a reader transaction that will not be able to see what happens next.
        # We don't need to do anything with this; it just needs to exist.
        session2 = self.conn.open_session()
        session2.begin_transaction()

        # Now remove all data.
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor.set_key(ds.key(i))
            self.assertEqual(cursor.remove(), 0)
            self.session.commit_transaction()

        # Checkpoint.
        self.session.checkpoint()

        # Get the existing in-memory delete page skip statistic value.
        prev_cur_inmem_del_page_skip = self.get_stat(stat.conn.cursor_tree_walk_inmem_del_page_skip)

        # Now read the removed data.
        self.check(ds, 0, value_a)

        # Get the new in-memory delete page skip statistic value.
        cur_inmem_del_page_skip = self.get_stat(stat.conn.cursor_tree_walk_inmem_del_page_skip)

        self.assertGreater(cur_inmem_del_page_skip, prev_cur_inmem_del_page_skip)

        # Tidy up.
        session2.rollback_transaction()
        session2.close()
        cursor.close()

    # As above, but hold the reader's snapshot minimum below the delete transactions with an
    # unrelated long-running transaction. The page's stop transactions then fall in a committed gap
    # between concurrent transactions: the snap_min bound alone cannot skip the page, but the
    # stop-transaction range check proves every stop committed before the snapshot and can.
    def test_checkpoint_committed_gap(self):
        # Avoid checkpoint error with precise checkpoint
        if self.ckpt_config == 'precise_checkpoint=true':
            self.conn.set_timestamp('stable_timestamp=1')

        uri = 'table:checkpoint32_gap'
        nrows = 1000

        # Create a table.
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()

        value_a = "aaaaa" * 100

        # Write some initial data.
        cursor = self.session.open_cursor(ds.uri, None, None)
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor[ds.key(i)] = value_a
            self.session.commit_transaction()

        # Start a long-running transaction that writes to an unrelated table. The write allocates a
        # transaction id below the deletes that follow; leaving it open both pins the oldest id (so
        # the tombstones are retained at reconciliation) and holds later readers' snapshot minimum
        # below the delete transactions.
        gap_uri = 'table:checkpoint32_gap_other'
        self.session.create(gap_uri, 'key_format=S,value_format=S')
        session_gap = self.conn.open_session()
        session_gap.begin_transaction()
        gap_cursor = session_gap.open_cursor(gap_uri)
        gap_cursor['gapkey'] = 'gapval'

        # Now remove all data. These commits use transaction ids above the open gap transaction.
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor.set_key(ds.key(i))
            self.assertEqual(cursor.remove(), 0)
            self.session.commit_transaction()

        # Checkpoint.
        self.session.checkpoint()

        # Get the existing in-memory delete page skip statistic value.
        prev_cur_inmem_del_page_skip = self.get_stat(stat.conn.cursor_tree_walk_inmem_del_page_skip)

        # Read the removed data while the gap transaction is still open, so the reader's snapshot
        # minimum sits below the (committed and visible) delete transactions.
        self.check(ds, 0, value_a)

        # Get the new in-memory delete page skip statistic value.
        cur_inmem_del_page_skip = self.get_stat(stat.conn.cursor_tree_walk_inmem_del_page_skip)

        self.assertGreater(cur_inmem_del_page_skip, prev_cur_inmem_del_page_skip)

        # Tidy up.
        gap_cursor.close()
        session_gap.rollback_transaction()
        session_gap.close()
        cursor.close()

    # Skip fully-deleted pages during the tree walk while they are on disk (the WT_REF_DISK path
    # that reads the stop time aggregate from the parent's address cell), as opposed to the
    # in-memory page-modify path exercised above.
    def test_checkpoint_ondisk_skip(self):
        # Avoid checkpoint error with precise checkpoint
        if self.ckpt_config == 'precise_checkpoint=true':
            self.conn.set_timestamp('stable_timestamp=1')

        uri = 'table:checkpoint32_ondisk'
        nrows = 1000

        # Create a table.
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()

        value_a = "aaaaa" * 100

        # Write some initial data.
        cursor = self.session.open_cursor(ds.uri, None, None)
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor[ds.key(i)] = value_a
            self.session.commit_transaction()

        # Hold a reader open so the tombstones are not globally visible at checkpoint time; this
        # forces the deleted pages to be written to disk with their stop time aggregate rather than
        # discarded from the tree.
        session2 = self.conn.open_session()
        session2.begin_transaction()

        # Now remove all data.
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor.set_key(ds.key(i))
            self.assertEqual(cursor.remove(), 0)
            self.session.commit_transaction()

        # Checkpoint, then drop the cache so the deleted leaf pages are read back from disk. After
        # the reopen the holding reader is gone, so the deletes are visible to a fresh reader.
        self.session.checkpoint()
        session2.rollback_transaction()
        session2.close()
        cursor.close()
        self.reopen_conn()

        # Get the existing on-disk delete page skip statistic value.
        prev_cur_del_page_skip = self.get_stat(stat.conn.cursor_tree_walk_del_page_skip)

        # Now read the removed data; the deleted pages are skipped from their on-disk address cells.
        self.check(ds, 0, value_a)

        # Get the new on-disk delete page skip statistic value.
        cur_del_page_skip = self.get_stat(stat.conn.cursor_tree_walk_del_page_skip)

        self.assertGreater(cur_del_page_skip, prev_cur_del_page_skip)

if __name__ == '__main__':
    wttest.run()
