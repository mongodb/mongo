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

import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_fast_truncate import LayeredFastTruncateConfigMixin
from wiredtiger import stat
from wtscenario import make_scenarios


@disagg_test_class
class test_layered_fast_truncate24(LayeredFastTruncateConfigMixin, wttest.WiredTigerTestCase):
    """Exercise deleted internal-page skips on an oplog-shaped layered table."""

    conn_config = ('cache_size=1GB,statistics=(all),precise_checkpoint=true,'
        'checkpoint_cleanup=(wait=100000),disaggregated=(role="leader"),')
    uri = 'table:test_layered_fast_truncate24'
    table_config = ('key_format=i,value_format=S,block_manager=disagg,type=layered,'
        'log=(enabled=false),allocation_size=512,leaf_page_max=512,'
        'internal_page_max=512,memory_page_max=4096')
    nrows = 1000
    value = 'a' * 50

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def populate(self):
        self.session.create(self.uri, self.table_config)
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        with (
            wttest.open_cursor(self.session, self.uri) as cursor,
            self.transaction(commit_timestamp=10),
        ):
            for key in range(1, self.nrows + 1):
                cursor[key] = self.value
        self.leader_checkpoint(10)

    def evict_leaves(self):
        with (
            wttest.open_cursor(
                self.session, self.uri, config='debug=(release_evict)') as cursor,
            self.transaction(read_timestamp=10, rollback=True),
        ):
            for key in range(1, self.nrows + 1):
                cursor.set_key(key)
                self.assertEqual(cursor.search(), 0)
                cursor.reset()

    def truncate_on(self, session, start_key, stop_key, commit_ts):
        with (
            wttest.open_cursor(session, self.uri) as start,
            wttest.open_cursor(session, self.uri) as stop,
            self.transaction(session=session, commit_timestamp=commit_ts),
        ):
            start.set_key(start_key)
            stop.set_key(stop_key)
            session.truncate(None, start, stop, None)

    def scan_keys(self, session, read_ts=None):
        keys = []
        config = None if read_ts is None else 'read_timestamp=' + self.timestamp_str(read_ts)
        session.begin_transaction(config)
        with wttest.open_cursor(session, self.uri) as cursor:
            while cursor.next() == 0:
                keys.append(cursor.get_key())
        session.rollback_transaction()
        return keys

    def skip_stats(self, conn):
        return (
            self.get_stat(stat.conn.cursor_tree_walk_ondisk_del_internal_page_skip, conn=conn),
            self.get_stat(
                stat.conn.cursor_tree_walk_read_only_del_internal_page_skip, conn=conn),
        )

    def test_leader_keeps_inmem_deleted_stable_subtree_after_checkpoint(self):
        self.populate()
        self.evict_leaves()

        fast_before = self.get_stat(stat.conn.cursor_truncate_fast_delete_pages)
        self.truncate_on(self.session, 100, 900, 20)
        self.assertGreater(self.get_stat(stat.conn.cursor_truncate_fast_delete_pages), fast_before)
        expected = list(range(1, 100)) + list(range(901, self.nrows + 1))
        self.assertEqual(self.scan_keys(self.session, 25), expected)

        # The leader's stable table is writable, so an in-memory internal page is not skipped.
        self.leader_checkpoint(20)
        _, inmem_before = self.skip_stats(self.conn)
        self.assertEqual(self.scan_keys(self.session, 25), expected)
        _, inmem_after = self.skip_stats(self.conn)
        self.assertEqual(inmem_after, inmem_before,
            'leader skipped an in-memory deleted internal subtree in stable')

    def test_follower_truncate_positions_across_deleted_stable_subtree(self):
        self.populate()
        self.evict_leaves()

        self.truncate_on(self.session, 1, 800, 20)
        self.assertGreater(self.get_stat(stat.conn.cursor_truncate_fast_delete_pages), 0)
        self.assertEqual(self.scan_keys(self.session, 25), list(range(801, self.nrows + 1)))
        self.leader_checkpoint(20)

        conn_follow, session_follow = self.open_follower(self.table_config)
        try:
            self.assertEqual(
                self.scan_keys(session_follow, 10), list(range(1, self.nrows + 1)))

            inmem_before = self.skip_stats(conn_follow)[1]
            self.assertEqual(
                self.scan_keys(session_follow, 25), list(range(801, self.nrows + 1)))
            inmem_after = self.skip_stats(conn_follow)[1]
            self.assertGreater(inmem_after, inmem_before,
                'follower did not skip a deleted in-memory stable subtree')

            disk_before, inmem_before = self.skip_stats(conn_follow)
            fast_before = self.get_stat(
                stat.conn.cursor_truncate_fast_delete_pages, conn=conn_follow)

            self.truncate_on(session_follow, 1, 950, 30)

            disk_after, inmem_after = self.skip_stats(conn_follow)
            fast_after = self.get_stat(
                stat.conn.cursor_truncate_fast_delete_pages, conn=conn_follow)
            self.assertGreater(
                disk_after + inmem_after, disk_before + inmem_before,
                'follower boundary positioning did not skip a deleted stable subtree')
            self.assertEqual(fast_after, fast_before,
                'follower truncate unexpectedly fast-deleted a stable page')
            self.assertEqual(self.scan_keys(session_follow), list(range(951, self.nrows + 1)))
        finally:
            session_follow.close()
            conn_follow.close()


if __name__ == '__main__':
    wttest.run()
