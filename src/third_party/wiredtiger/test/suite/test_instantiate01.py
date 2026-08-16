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
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_instantiate01.py
#
# Instantiating updates while reading a page from disk is internal bookkeeping: the page ends up
# clean, so it must not mark the btree dirty. If it did, the next checkpoint would rewrite a tree
# with no user-visible changes. Cover the two instantiation paths: fast-truncated pages and pages
# with prepared updates on disk.
@wttest.skip_for_hook("disagg", "checkpoint skip behavior differs on disagg followers")
@wttest.skip_for_hook("tiered", "flush_tier calls interfere with checkpoint skip accounting")
class test_instantiate01(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all)'

    format_values = [
        ('column', dict(key_format='r')),
        ('row', dict(key_format='i')),
    ]

    scenarios = make_scenarios(format_values)

    def checkpoint_handles_applied(self):
        self.session.checkpoint()
        return self.get_stat(stat.conn.checkpoint_handle_applied)

    def checkpoint_until_quiescent(self):
        # A tree is only skipped by checkpoints once it is clean and its file has no reclaimable
        # space, which can take a few checkpoints after real work. Checkpoint until nothing is
        # applied so the assertions below observe only the effect of the instantiating read.
        for _ in range(10):
            if self.checkpoint_handles_applied() == 0:
                return
        self.fail('checkpoints never reached a quiescent state')

    def test_instantiate_deleted_page(self):
        nrows = 20000
        uri = 'table:instantiate01'
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format='S',
            config='log=(enabled=false)')
        ds.populate()

        # Make the values distinct: identical column-store values are RLE-compressed into a
        # single cell, which would collapse the tree to a page or two and leave nothing for
        # fast-truncate to delete.
        def value(i):
            return 'a' * 100 + str(i)

        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Write data at time 10 and persist it.
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[ds.key(i)] = value(i)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        # Reopen the connection so nothing is in memory and the truncate can be fast.
        self.reopen_conn()
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))

        # Fast-truncate the middle of the table at time 20.
        self.session.begin_transaction()
        lo_cursor = self.session.open_cursor(uri)
        hi_cursor = self.session.open_cursor(uri)
        lo_cursor.set_key(ds.key(nrows // 4 + 1))
        hi_cursor.set_key(ds.key(nrows // 4 + nrows // 2))
        self.assertEqual(self.session.truncate(None, lo_cursor, hi_cursor, None), 0)
        lo_cursor.close()
        hi_cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        self.assertGreater(self.get_stat(stat.conn.rec_page_delete_fast), 0)

        # Persist the truncate and checkpoint until the tree is skipped as clean.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.checkpoint_until_quiescent()

        # Read everything at time 10. The truncate isn't visible at that timestamp and isn't
        # globally visible (oldest is pinned at 1), so this read must instantiate tombstones
        # onto the truncated pages.
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(10))
        count = 0
        for k, v in cursor:
            self.assertEqual(v, value(count + 1))
            count += 1
        self.assertEqual(count, nrows)
        self.session.rollback_transaction()
        cursor.close()
        self.assertGreater(self.get_stat(stat.conn.cache_read_deleted), 0)

        # Instantiation is not a user-visible change: the tree must still be clean and the next
        # checkpoint must skip it entirely.
        self.assertEqual(self.checkpoint_handles_applied(), 0)

    def test_instantiate_prepared_updates(self):
        uri = 'table:instantiate01'
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format='S',
            config='log=(enabled=false)')
        ds.populate()

        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Commit one key, then prepare an update to a neighboring key on the same page.
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(uri)
        session2.begin_transaction()
        cursor2[ds.key(2)] = 'committed'
        session2.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        session3 = self.conn.open_session()
        cursor3 = session3.open_cursor(uri)
        session3.begin_transaction()
        cursor3[ds.key(1)] = 'prepared'
        session3.prepare_transaction('prepare_timestamp=' + self.timestamp_str(20))
        cursor2.close()

        # Evict the page: reconciliation writes the prepared update to disk and the page is
        # rebuilt in cache from that disk image, instantiating the prepared update onto the
        # update chain. (The page can't leave the cache entirely while the prepared transaction
        # is active, so the instantiate-on-disk-read variant of this path is not reachable here;
        # it is exercised by the disagg tests.)
        evict_cursor = self.session.open_cursor(uri, None, 'debug=(release_evict)')
        evict_cursor.set_key(ds.key(2))
        self.assertEqual(evict_cursor.search(), 0)
        self.assertEqual(evict_cursor.reset(), 0)
        evict_cursor.close()

        # Checkpoint until the tree is skipped as clean.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.checkpoint_until_quiescent()

        # Read the committed key (avoiding a prepare conflict) and checkpoint again: the tree
        # holding only the instantiated prepared update must stay clean and be skipped.
        cursor = self.session.open_cursor(uri)
        cursor.set_key(ds.key(2))
        self.assertEqual(cursor.search(), 0)
        cursor.close()
        self.assertEqual(self.checkpoint_handles_applied(), 0)

        session3.rollback_transaction()
        session3.close()
        session2.close()
