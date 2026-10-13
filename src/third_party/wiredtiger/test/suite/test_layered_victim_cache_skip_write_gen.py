#!/usr/bin/env python3
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

# Rolled-back updates and eviction must preserve checkpointed values across role switches.

import wttest
from helper_disagg import DisaggSizeTestMixin, disagg_test_class
from wiredtiger import stat


@disagg_test_class
class test_layered_victim_cache_skip_write_gen(DisaggSizeTestMixin, wttest.WiredTigerTestCase):
    test_name = __qualname__
    conn_config = (
        'disaggregated=(role="leader",lose_all_my_data=true),'
        'cache_size=2GB,statistics=(all),precise_checkpoint=true'
    )
    disagg_config = 'victim_cache_max_entries=10000'

    uri = 'layered:' + test_name
    stable_uri = 'file:' + test_name + '.wt_stable'
    table_config = 'key_format=S,value_format=S,leaf_page_max=4KB'

    nrows = 10
    value = 'A' * 1024

    def key(self, i):
        return 'key{:08d}'.format(i)

    def evict_page(self, key):
        evict = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        self.session.begin_transaction()
        evict.set_key(key)
        evict.search_near()
        evict.reset()
        evict.close()
        self.session.rollback_transaction()

    def test_skip_write_gen_survives_victim_cache(self):
        self.conn.set_timestamp('oldest_timestamp=1,stable_timestamp=1')
        self.session.create(self.uri, self.table_config)

        # Persist a full image: this block is what a later skip-write retains.
        self.session.begin_transaction()
        c = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            c[self.key(i)] = self.value
        c.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        # Drop the pages so the next reads load them from the checkpointed blocks.
        for i in range(self.nrows):
            self.evict_page(self.key(i))
        self.session.checkpoint()

        # An aborted update dirties each page without changing its content, so
        # eviction keeps the checkpointed block; a second, clean eviction then
        # hands the retained image to the victim cache.
        skips = self.get_conn_stat(stat.conn.rec_skip_write)
        puts = self.get_conn_stat(stat.conn.block_cache_puts)
        for i in range(self.nrows):
            self.session.begin_transaction()
            c = self.session.open_cursor(self.uri)
            c[self.key(i)] = 'D' * 1024
            c.close()
            self.session.rollback_transaction()
            self.evict_page(self.key(i))
        for i in range(self.nrows):
            self.evict_page(self.key(i))
        self.assertGreater(self.get_conn_stat(stat.conn.rec_skip_write), skips)
        self.assertGreater(self.get_conn_stat(stat.conn.block_cache_puts), puts)

        # Verify the checkpoint and read the original values after switching roles.
        self.conn.reconfigure('disaggregated=(role="follower")')
        self.conn.reconfigure('disaggregated=(role="leader")')
        self.retryEBUSY(self.session, lambda: self.session.verify(self.stable_uri))

        for i in range(self.nrows):
            c = self.session.open_cursor(self.uri)
            c.set_key(self.key(i))
            self.assertEqual(c.search(), 0)
            self.assertEqual(c.get_value(), self.value)
            c.close()
