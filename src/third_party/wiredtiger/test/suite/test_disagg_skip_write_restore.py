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
from helper_disagg import DisaggSizeTestMixin, disagg_test_class

# test_disagg_skip_write_restore.py
#    A page can come back from disk with no rows left on it, because the deletes
# recorded against it have become obsolete. Reconciling such a page writes
# nothing at all, and it still has to keep pointing at the block it already has.
@disagg_test_class
class test_disagg_skip_write_restore(DisaggSizeTestMixin, wttest.WiredTigerTestCase):

    uri_base = 'test_disagg_skip_write_restore'
    conn_config = (
        'disaggregated=(role="leader",lose_all_my_data=true),'
        'cache_size=2GB,statistics=(all),precise_checkpoint=true,'
        # A delete leaves a page smaller than the delta describing it, so allow
        # a delta to exceed the image it applies to; otherwise these pages are
        # rewritten whole and never come back from disk empty.
        'page_delta=(delta_pct=1000,leaf_page_delta=true)'
    )
    uri = 'layered:' + uri_base
    table_config = 'key_format=S,value_format=S,leaf_page_max=4KB'

    nrows = 100
    value = 'A' * 1024

    def key(self, i):
        return 'key{:08d}'.format(i)

    def evict_page(self, key, read_ts=None):
        evict = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        cfg = None if read_ts is None else 'read_timestamp=' + self.timestamp_str(read_ts)
        self.session.begin_transaction(cfg)
        evict.set_key(key)
        evict.search_near()
        evict.reset()
        evict.close()
        self.session.rollback_transaction()

    def test_emptied_page_keeps_its_block(self):
        self.conn.set_timestamp('oldest_timestamp=1,stable_timestamp=1')
        self.session.create(self.uri, self.table_config)

        self.session.begin_transaction()
        c = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            c[self.key(i)] = self.value
        c.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        # Delete everything and make it stable, but leave oldest behind, so the
        # deletes are still worth recording rather than simply dropping.
        self.session.begin_transaction()
        c = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            c.set_key(self.key(i))
            c.remove()
        c.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.session.checkpoint()

        deltas = self.get_conn_stat(stat.conn.rec_page_delta_leaf)
        self.assertGreater(deltas, 0, 'the deletes were not recorded as deltas')

        # Drop the now-clean pages. Position back before the deletes, otherwise
        # there is nothing left to position on.
        for i in range(self.nrows):
            self.evict_page(self.key(i), read_ts=10)

        # Only now do the deletes become obsolete, so reading the pages back
        # rebuilds them with none of the deleted rows left on them.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(20))
        c = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            c.set_key(self.key(i))
            c.search_near()
        c.close()

        self.assertGreater(self.get_conn_stat(stat.conn.cache_read_leaf_delta), 0,
            'the pages were not rebuilt from deltas')

        restore_before = self.get_conn_stat(stat.conn.cache_write_restore_invisible)
        skip_before = self.get_conn_stat(stat.conn.rec_skip_write)

        # Rewrite every key past the stable timestamp. The pages now hold
        # nothing a reconciliation is allowed to write, so every one of them
        # goes back on the in-memory chain and no block is produced.
        self.session.begin_transaction()
        c = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            c[self.key(i)] = 'B' * 1024
        c.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        for i in range(0, self.nrows, 3):
            self.evict_page(self.key(i))

        restored = self.get_conn_stat(stat.conn.cache_write_restore_invisible) - restore_before
        skipped = self.get_conn_stat(stat.conn.rec_skip_write) - skip_before

        self.assertGreater(restored, 0)

        # Having written nothing, each of those reconciliations has to say it is
        # still using the block the page already had.
        self.assertEqual(skipped, restored)

        # The rewritten values are still all there.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        self.session.checkpoint()
        c = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            c.set_key(self.key(i))
            self.assertEqual(c.search(), 0)
            self.assertEqual(c.get_value(), 'B' * 1024)
        c.close()
