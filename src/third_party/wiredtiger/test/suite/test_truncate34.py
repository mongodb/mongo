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
from wiredtiger import stat

# Truncating a range whose rows are all already deleted must not fast-delete the
# pages: the on-disk images fully express the deletion, and page-delete
# information would only duplicate it. The duplicate state is what a later
# reconciliation that skips the write can discard while the parent still holds a
# fast-truncate proxy cell that needs it.
@wttest.skip_for_hook("tiered", "tiered truncate does not take the fast-delete path")
class test_truncate34(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB,statistics=(all)'
    uri = 'table:test_truncate34'
    create_cfg = 'key_format=i,value_format=S,leaf_page_max=4KB'

    value = 'abcdefghijklmnopqrstuvwxyz' * 3
    nrows = 2000

    def get_stat(self, statname):
        c = self.session.open_cursor('statistics:')
        val = c[statname][2]
        c.close()
        return val

    def evict_all(self):
        # Read below the deletes so the search lands on every page.
        ev = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(15))
        for i in range(1, self.nrows + 1):
            ev.set_key(i)
            ev.search()
            ev.reset()
        self.session.rollback_transaction()
        ev.close()

    def test_truncate_already_deleted(self):
        self.session.create(self.uri, self.create_cfg)
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))

        # Insert rows, then individually delete a band in the middle: the truncate
        # below starts on a live key and walks across fully-deleted leaves to reach
        # live ones, so it must skip the former and fast-delete the latter. (Dead
        # keys at the range edges are never walked: the session positions the
        # cursors on visible keys.)
        dead_lo, dead_hi = self.nrows // 4, 3 * self.nrows // 4
        c = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            self.session.begin_transaction()
            c[i] = self.value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        for i in range(dead_lo, dead_hi + 1):
            self.session.begin_transaction()
            c.set_key(i)
            self.assertEqual(c.remove(), 0)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        c.close()

        # Make the per-key stops durable in the page images and push the pages to disk.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.session.checkpoint()
        self.evict_all()

        # Truncate the fully-deleted range.
        # Start at key 5 so this is a range truncate: a truncate spanning the whole
        # table takes a different, tree-level path that never visits the pages.
        fast_before = self.get_stat(stat.conn.rec_page_delete_fast)
        self.session.begin_transaction()
        start = self.session.open_cursor(self.uri)
        start.set_key(5)
        self.session.truncate(None, start, None, None)
        start.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        # The live leaves fast-delete; the fully-deleted leaves must be skipped.
        self.assertGreater(self.get_stat(stat.conn.rec_page_delete_fast), fast_before,
                           'no live pages were fast-deleted; the walk did not run')
        self.assertGreater(self.get_stat(stat.conn.rec_page_delete_fast_skip_deleted), 0,
                           'no fully-deleted pages were skipped; the case was not exercised')

        # Correctness across timestamps: values before the deletes, gone after.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(15))
        c = self.session.open_cursor(self.uri)
        c.set_key(1)
        self.assertEqual(c.search(), 0)
        self.assertEqual(c.get_value(), self.value)
        c.close()
        self.session.rollback_transaction()

        # At 25: the middle band is deleted, the edges are live. At 35: all gone.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(25))
        c = self.session.open_cursor(self.uri)
        c.set_key(self.nrows // 2)
        self.assertEqual(c.search(), wiredtiger.WT_NOTFOUND)
        c.set_key(10)
        self.assertEqual(c.search(), 0)
        c.close()
        self.session.rollback_transaction()
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(35))
        c = self.session.open_cursor(self.uri)
        for i in [10, self.nrows // 2, self.nrows]:
            c.set_key(i)
            self.assertEqual(c.search(), wiredtiger.WT_NOTFOUND)
        c.close()
        self.session.rollback_transaction()

        # The pages survive a further checkpoint and eviction cycle.
        self.session.checkpoint()
        self.evict_all()
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(15))
        c = self.session.open_cursor(self.uri)
        c.set_key(self.nrows)
        self.assertEqual(c.search(), 0)
        c.close()
        self.session.rollback_transaction()
