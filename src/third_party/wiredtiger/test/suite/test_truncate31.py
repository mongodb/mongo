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
from wiredtiger import stat, WiredTigerError, wiredtiger_strerror, WT_ROLLBACK, WT_NOTFOUND
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_truncate31.py
#    A fast-truncate over a very large range pulls in and dirties the parent
#    internal pages of every fast-deleted leaf. Those internal pages cannot be
#    reconciled until the truncate becomes stable, so if the truncate is not
#    globally visible they pin dirty cache indefinitely. A single truncate that
#    would pin enough dirty content to approach the cache's dirty eviction
#    threshold is rolled back (WT_ROLLBACK) rather than being allowed to wedge
#    the system. This mimics the oplog-truncation stall from HELP-96307.
# The fast-truncate dirty-cache accounting relies on on-disk leaf pages, local
# eviction_dirty_trigger semantics, and reopen_conn, none of which behave the
# same under the tiered or disaggregated storage hooks.
@wttest.skip_for_hook("tiered", "fast-truncate dirty-cache accounting is not meaningful under tiered storage")
@wttest.skip_for_hook("disagg", "fast-truncate dirty-cache accounting is not meaningful under disaggregated storage")
class test_truncate31(wttest.WiredTigerTestCase):
    # A small cache with a low dirty trigger so the accumulated dirty internal
    # pages from a large truncate cross the per-transaction threshold quickly.
    conn_config = 'cache_size=20MB,statistics=(all),' \
        'eviction_dirty_target=1,eviction_dirty_trigger=2'

    # Small page sizes so a modestly sized table spans many leaf pages and hence
    # many parent internal pages, which is what a fast-truncate pins in cache.
    table_config = 'allocation_size=512B,leaf_page_max=2KB,internal_page_max=2KB'

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('integer_row', dict(key_format='i', value_format='S')),
    ]
    scenarios = make_scenarios(format_values)

    def large_truncate(self, uri, lo, hi):
        # Truncate [lo, hi) using a bounded transaction. Returns WT_ROLLBACK if
        # the operation was aborted, 0 on success.
        self.session.begin_transaction()
        start = self.session.open_cursor(uri, None)
        start.set_key(lo)
        end = self.session.open_cursor(uri, None)
        end.set_key(hi)
        try:
            self.session.truncate(None, start, end, None)
        except WiredTigerError as e:
            start.close()
            end.close()
            self.session.rollback_transaction()
            if wiredtiger_strerror(WT_ROLLBACK) in str(e):
                return WT_ROLLBACK
            raise
        start.close()
        end.close()
        self.session.commit_transaction()
        return 0

    def populate_on_disk(self, uri, nrows):
        # Populate a table and push its leaf pages to disk so a later truncate
        # can fast-delete them. Returns the dataset handle.
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format,
            value_format=self.value_format, config=self.table_config)
        ds.populate()
        cursor = self.session.open_cursor(uri)
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor[ds.key(i)] = str(i) + "abcde" * 8
            self.session.commit_transaction()
        cursor.close()
        return ds

    def combined_truncate(self, write_uri, write_ds, write_keys,
            trunc_uri, trunc_ds, lo, hi):
        # In a single transaction, apply a set of writes to one table and then
        # truncate a range of another. Returns WT_ROLLBACK if the truncate
        # aborted (leaving the transaction in its error state for the caller to
        # inspect and roll back), or 0 after committing on success.
        self.session.begin_transaction()
        wcursor = self.session.open_cursor(write_uri)
        for i in write_keys:
            wcursor[write_ds.key(i)] = "combined" + str(i)
        wcursor.close()

        start = self.session.open_cursor(trunc_uri, None)
        start.set_key(trunc_ds.key(lo))
        end = self.session.open_cursor(trunc_uri, None)
        end.set_key(trunc_ds.key(hi))
        try:
            self.session.truncate(None, start, end, None)
        except WiredTigerError as e:
            start.close()
            end.close()
            if wiredtiger_strerror(WT_ROLLBACK) in str(e):
                return WT_ROLLBACK
            raise
        start.close()
        end.close()
        self.session.commit_transaction()
        return 0

    def test_truncate_combined_abort(self):
        # A transaction that mixes ordinary writes with a large truncate: when
        # the truncate trips the dirty-cache limit the entire transaction must
        # abort, discarding the ordinary writes too. A later, smaller
        # transaction must be unaffected (the per-txn counter does not leak).
        trunc_uri = 'table:oplog_combined'
        side_uri = 'table:side_combined'
        nrows = 300000
        side_nrows = 2000

        trunc_ds = self.populate_on_disk(trunc_uri, nrows)
        side_ds = self.populate_on_disk(side_uri, side_nrows)

        self.session.checkpoint()
        self.reopen_conn()

        # Keys we add to the side table inside the doomed transaction. They live
        # above the populated range so they are guaranteed absent beforehand.
        combined_keys = list(range(side_nrows + 1, side_nrows + 51))

        # Model the stall: a concurrent long-running transaction keeps the
        # truncate from becoming globally visible.
        session2 = self.conn.open_session()
        session2.begin_transaction()

        # The rollback also emits a warning naming the truncate as the cause.
        with self.expectedStdoutPattern('rolling back a truncate that is pinning too much dirty cache'):
            ret = self.combined_truncate(side_uri, side_ds, combined_keys,
                trunc_uri, trunc_ds, 1, nrows)
        self.assertEqual(ret, WT_ROLLBACK)

        # The transaction is now in its error state: committing it fails, and
        # the failed commit rolls the transaction back so the session is reusable.
        with self.expectedStderrPattern('transaction requires rollback'):
            self.assertRaisesException(WiredTigerError,
                lambda: self.session.commit_transaction())

        # The ordinary writes in the aborted transaction must not have persisted.
        vcursor = self.session.open_cursor(side_uri)
        for i in combined_keys:
            vcursor.set_key(side_ds.key(i))
            self.assertEqual(vcursor.search(), WT_NOTFOUND)
        vcursor.close()

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        truncate_rollbacks = \
            stat_cursor[stat.conn.txn_truncate_dirty_cache_rollback][2]
        stat_cursor.close()
        self.assertEqual(truncate_rollbacks, 1)

        # Independence: a subsequent small transaction that combines a modest
        # truncate with a write must commit cleanly and not trip the limit. The
        # per-transaction counter from the aborted transaction must not leak.
        small_keys = list(range(side_nrows + 51, side_nrows + 61))
        ret = self.combined_truncate(side_uri, side_ds, small_keys,
            side_uri, side_ds, 1, 100)
        self.assertEqual(ret, 0)

        # Its effects must persist, and the rollback stat must be unchanged.
        vcursor = self.session.open_cursor(side_uri)
        for i in small_keys:
            vcursor.set_key(side_ds.key(i))
            self.assertEqual(vcursor.search(), 0)
        vcursor.close()

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        truncate_rollbacks = \
            stat_cursor[stat.conn.txn_truncate_dirty_cache_rollback][2]
        stat_cursor.close()
        self.assertEqual(truncate_rollbacks, 1)

        session2.rollback_transaction()
        session2.close()

    def test_truncate_abort(self):
        uri = 'table:oplog'
        nrows = 300000

        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format,
            value_format=self.value_format, config=self.table_config)
        ds.populate()

        # Distinct values (so VLCS doesn't condense the table via RLE) written in
        # their own transactions so pages become clean and evictable as we go.
        cursor = self.session.open_cursor(uri)
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor[ds.key(i)] = str(i) + "abcde" * 8
            self.session.commit_transaction()
        cursor.close()

        # Checkpoint and reopen so the leaf pages live on disk, making them
        # eligible for fast-truncate (which deletes them without reading them).
        self.session.checkpoint()
        self.reopen_conn()

        # A concurrent long-running transaction models the real stall: the
        # truncate is not globally visible, so its freshly dirtied parent pages
        # cannot be reconciled or evicted and stay pinned in cache. The abort
        # itself is driven purely by the accumulated dirty-byte total, not by
        # visibility, so it fires during the truncate walk regardless.
        session2 = self.conn.open_session()
        session2.begin_transaction()

        # Truncate the entire range in a single transaction. The dirty internal
        # pages pinned by the truncate should cross the threshold and force the
        # transaction to roll back.
        # The rollback also emits a warning naming the truncate as the cause.
        with self.expectedStdoutPattern('rolling back a truncate that is pinning too much dirty cache'):
            ret = self.large_truncate(uri, ds.key(1), ds.key(nrows))
        self.assertEqual(ret, WT_ROLLBACK)

        # We should have fast-deleted at least one page on the way to the stall,
        # and the rollback should be reflected in the tracking statistic.
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        fastdelete_pages = stat_cursor[stat.conn.rec_page_delete_fast][2]
        truncate_rollbacks = \
            stat_cursor[stat.conn.txn_truncate_dirty_cache_rollback][2]
        stat_cursor.close()
        self.assertGreater(fastdelete_pages, 0)
        self.assertEqual(truncate_rollbacks, 1)

        session2.rollback_transaction()
        session2.close()

    def test_truncate_small_ok(self):
        # A control case: a small truncate stays well under the threshold and
        # commits normally.
        uri = 'table:oplog_small'
        nrows = 2000

        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format,
            value_format=self.value_format, config=self.table_config)
        ds.populate()

        cursor = self.session.open_cursor(uri)
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor[ds.key(i)] = str(i) + "abcde" * 8
            self.session.commit_transaction()
        cursor.close()

        self.session.checkpoint()
        self.reopen_conn()

        ret = self.large_truncate(uri, ds.key(1), ds.key(nrows))
        self.assertEqual(ret, 0)

        # A truncate that stays under the threshold must not be rolled back.
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        truncate_rollbacks = \
            stat_cursor[stat.conn.txn_truncate_dirty_cache_rollback][2]
        stat_cursor.close()
        self.assertEqual(truncate_rollbacks, 0)
