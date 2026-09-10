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

# test_disagg_skip_write_restore02.py
#    A page can split in memory to make room for a burst of appends before any of that
# content is stable enough to write. While the split is pending, the original page still
# holds none of the content in a form reconciliation can write, and it must not answer
# with the block from before the split: some of the rows that block described have since
# moved to the new sibling the split created.
@disagg_test_class
class test_disagg_skip_write_restore02(DisaggSizeTestMixin, wttest.WiredTigerTestCase):

    uri_base = 'test_disagg_skip_write_restore02'
    conn_config = (
        'disaggregated=(role="leader",lose_all_my_data=true),'
        'cache_size=2GB,statistics=(all),precise_checkpoint=true,'
        'page_delta=(delta_pct=1000,leaf_page_delta=true),checkpoint_threads=1'
    )
    uri = 'layered:' + uri_base
    # A small in-memory limit makes the append burst below cross the in-memory split
    # threshold without needing an unwieldy number of rows.
    table_config = 'key_format=S,value_format=S,leaf_page_max=4KB,memory_page_max=4KB'

    nrows = 70

    def key(self, i):
        return 'key{:08d}a{:04d}'.format(0, i)

    def evict_page(self, key, read_ts=None):
        evict = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        cfg = None if read_ts is None else 'read_timestamp=' + self.timestamp_str(read_ts)
        self.session.begin_transaction(cfg)
        evict.set_key(key)
        evict.search_near()
        evict.reset()
        evict.close()
        self.session.rollback_transaction()

    def test_split_page_keeps_no_stale_block(self):
        self.conn.set_timestamp('oldest_timestamp=1,stable_timestamp=1')
        self.session.create(self.uri, self.table_config)

        # A durable row gives the page a block address, then its deletion becomes stable
        # too, so the block correctly describes an empty page from here on.
        self.session.begin_transaction()
        c = self.session.open_cursor(self.uri)
        c['key00000000'] = 'A' * 100
        c.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        self.session.begin_transaction()
        c = self.session.open_cursor(self.uri)
        c.set_key('key00000000')
        c.remove()
        c.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.session.checkpoint()
        self.evict_page('key00000000', read_ts=10)
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(20))
        c = self.session.open_cursor(self.uri)
        c.set_key('key00000000')
        c.search_near()
        c.close()

        # Append enough small, not-yet-stable rows onto the same page to force an
        # in-memory split before any of them can be written.
        self.session.begin_transaction()
        c = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            c[self.key(i)] = 'B' * 50
        c.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        # Evict repeatedly: this both drives the in-memory split and, once one has
        # happened, repeatedly reconciles the split-off original page while it still
        # holds only non-stable content. If the split isn't excluded from the decision to
        # keep the previous block, one of these reconciliations wrongly preserves the
        # pre-split address.
        split_seen = False
        stale_block_kept = False
        for _ in range(60):
            inmem_before = self.get_conn_stat(stat.conn.cache_inmem_splittable)
            skip_before = self.get_conn_stat(stat.conn.rec_skip_write)
            self.evict_page('key00000000')
            inmem_after = self.get_conn_stat(stat.conn.cache_inmem_splittable)
            skip_after = self.get_conn_stat(stat.conn.rec_skip_write)
            if inmem_after > inmem_before:
                split_seen = True
            elif split_seen and skip_after > skip_before:
                stale_block_kept = True

        self.assertTrue(split_seen, 'the in-memory split never happened')
        self.assertFalse(stale_block_kept,
            'a page pending an in-memory split kept its pre-split block address')

        # The appended rows are still all there once they become stable.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        self.session.checkpoint()
        c = self.session.open_cursor(self.uri)
        found = {}
        while c.next() == 0:
            found[c.get_key()] = c.get_value()
        c.close()
        self.assertEqual(set(found.keys()), set(self.key(i) for i in range(self.nrows)))
        for value in found.values():
            self.assertEqual(value, 'B' * 50)
