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

# test_layered_fast_truncate03.py
#   Tests that a follower correctly handles pages that were fast-truncated on the
#   leader: stable pages must never be dirtied, and deleted state must survive
#   eviction and reopen.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wiredtiger import stat

@disagg_test_class
class test_layered_fast_truncate03(wttest.WiredTigerTestCase):

    uri         = 'layered:test_layered_fast_truncate03'
    nrows       = 5000
    value       = 'a' * 500
    trunc_start = 1001
    trunc_stop  = 4000

    conn_config = 'cache_size=50MB,statistics=(all),disaggregated=(role="leader")'
    disagg_storages = gen_disagg_storages('test_layered_fast_truncate03', disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def setUp(self):
        if wiredtiger.disagg_fast_truncate_build() == 0:
            self.skipTest("fast truncate support is not enabled")
        super().setUp()

    def get_stat(self, conn, stat_key):
        s = conn.open_session('')
        val = s.open_cursor('statistics:')[stat_key][2]
        s.close()
        return val

    def leader_checkpoint(self, ts):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(ts) +
                                ',oldest_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()

    def setup_leader(self, extra_cfg=''):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        self.session.create(self.uri, 'key_format=i,value_format=S' + extra_cfg)
        cur = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            self.session.begin_transaction()
            cur[i] = self.value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        cur.close()
        self.leader_checkpoint(10)

        # Evict all pages before truncating so the leader uses page-level fast-delete markers.
        evict_cur = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        self.session.begin_transaction()
        for i in range(1, self.nrows + 1):
            evict_cur.set_key(i)
            evict_cur.search()
            evict_cur.reset()
        evict_cur.close()
        self.session.rollback_transaction()

    def truncate_and_checkpoint(self, trunc_start, trunc_stop, ts):
        # Fast-truncate rows [trunc_start, trunc_stop] on the leader and checkpoint.
        c_start = self.session.open_cursor(self.uri)
        c_start.set_key(trunc_start)
        c_stop = self.session.open_cursor(self.uri)
        c_stop.set_key(trunc_stop)
        self.session.begin_transaction()
        self.session.truncate(None, c_start, c_stop, None)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        c_start.close()
        c_stop.close()
        self.leader_checkpoint(ts)

    def open_follower(self):
        conn = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + ',create,cache_size=50MB,statistics=(all),disaggregated=(role="follower")')
        sess = conn.open_session('')
        sess.create(self.uri, 'key_format=i,value_format=S')
        self.disagg_advance_checkpoint(conn, self.conn)
        return conn, sess

    def advance_follower(self, conn):
        self.leader_checkpoint(20)
        self.disagg_advance_checkpoint(conn, self.conn)

    def evict_range(self, sess, start, stop, step=1):
        evict_cur = sess.open_cursor(self.uri, None, 'debug=(release_evict)')
        sess.begin_transaction('read_timestamp=' + self.timestamp_str(10))
        for i in range(start, stop + 1, step):
            evict_cur.set_key(i)
            evict_cur.search()
            evict_cur.reset()
        evict_cur.close()
        sess.rollback_transaction()

    def search_at(self, sess, key, ts):
        cur = sess.open_cursor(self.uri)
        txn_cfg = ('read_timestamp=' + self.timestamp_str(ts))
        sess.begin_transaction(txn_cfg)
        cur.set_key(key)
        ret = cur.search()
        val = cur.get_value() if ret == 0 else None
        sess.rollback_transaction()
        cur.close()
        return ret, val

    def test_no_dirty_on_read(self):
        # Reading fast-truncated pages on the follower must never dirty them. Verifies this holds
        # across a full load-evict-reload cycle for both single and bulk page reads.
        self.setup_leader()
        self.truncate_and_checkpoint(self.trunc_start, self.trunc_stop, 20)
        conn, sess = self.open_follower()
        sample = list(range(self.trunc_start, self.trunc_stop + 1, 10))
        dirty_before = self.get_stat(conn, stat.conn.cache_pages_dirty)

        # Initial read: no deleted key found, no pages dirtied.
        cur = sess.open_cursor(self.uri)
        sess.begin_transaction('read_timestamp=' + self.timestamp_str(25))
        for key in sample:
            cur.set_key(key)
            self.assertEqual(cur.search(), wiredtiger.WT_NOTFOUND)
        sess.rollback_transaction()
        cur.close()
        self.assertEqual(self.get_stat(conn, stat.conn.cache_pages_dirty), dirty_before)

        self.evict_range(sess, self.trunc_start, self.trunc_stop)

        # After eviction: reloaded pages still show no keys and remain clean.
        cur = sess.open_cursor(self.uri)
        sess.begin_transaction('read_timestamp=' + self.timestamp_str(25))
        for key in sample:
            cur.set_key(key)
            self.assertEqual(cur.search(), wiredtiger.WT_NOTFOUND)
        sess.rollback_transaction()
        cur.close()
        self.assertEqual(self.get_stat(conn, stat.conn.cache_pages_dirty), dirty_before)

        self.evict_range(sess, self.trunc_start, self.trunc_stop)
        sess.close()
        conn.close()

    def test_page_split_with_ingest_writes(self):
        # With small pages the truncated range spans many leaf pages. After ingest writes
        # restore a subset of truncated keys, those keys must be visible while the rest
        # remain deleted.
        self.setup_leader(',leaf_page_max=4096')
        self.truncate_and_checkpoint(self.trunc_start, self.trunc_stop, 20)
        conn, sess = self.open_follower()
        sample = list(range(self.trunc_start, self.trunc_stop + 1, 10))
        dirty_before = self.get_stat(conn, stat.conn.cache_pages_dirty)

        # No pages dirtied by reading the truncated range across many small leaf pages.
        cur = sess.open_cursor(self.uri)
        sess.begin_transaction('read_timestamp=' + self.timestamp_str(25))
        for key in sample:
            cur.set_key(key)
            self.assertEqual(cur.search(), wiredtiger.WT_NOTFOUND)
        sess.rollback_transaction()
        cur.close()
        self.assertEqual(self.get_stat(conn, stat.conn.cache_pages_dirty), dirty_before)

        self.evict_range(sess, self.trunc_start, self.trunc_stop)
        self.advance_follower(conn)

        # Write a subset of truncated keys to ingest.
        ingest_keys = set(sample[::3])
        cur = sess.open_cursor(self.uri)
        sess.begin_transaction()
        for key in ingest_keys:
            cur.set_key(key)
            cur.set_value(f'ingest_{key}')
            self.assertEqual(cur.insert(), 0)
        sess.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cur.close()

        # At ts=30: ingest keys are found; unwritten truncated keys remain deleted.
        cur = sess.open_cursor(self.uri)
        sess.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        for key in ingest_keys:
            cur.set_key(key)
            self.assertEqual(cur.search(), 0)
            self.assertEqual(cur.get_value(), f'ingest_{key}')
        for key in set(sample) - ingest_keys:
            cur.set_key(key)
            self.assertEqual(cur.search(), wiredtiger.WT_NOTFOUND)
        sess.rollback_transaction()
        cur.close()

        # At ts=25 (before the ingest write), all truncated keys must still be deleted.
        cur = sess.open_cursor(self.uri)
        sess.begin_transaction('read_timestamp=' + self.timestamp_str(25))
        for key in ingest_keys:
            cur.set_key(key)
            self.assertEqual(cur.search(), wiredtiger.WT_NOTFOUND)
        sess.rollback_transaction()
        cur.close()

        sess.close()
        conn.close()

    def test_state_preserved_on_reopen(self):
        # Closing and reopening the follower connection must not lose the deleted state.
        # The same checkpoint must still show truncated keys as WT_NOTFOUND after a cold start.
        self.setup_leader()
        self.truncate_and_checkpoint(self.trunc_start, self.trunc_stop, 20)

        truncated_keys     = [self.trunc_start, self.trunc_start + 100, self.trunc_stop]
        non_truncated_keys = [1, self.trunc_start - 1, self.trunc_stop + 1, self.nrows]

        def verify(sess):
            for key in truncated_keys:
                ret, _ = self.search_at(sess, key, 25)
                self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
            for key in non_truncated_keys:
                ret, val = self.search_at(sess, key, 25)
                self.assertEqual(ret, 0)
                self.assertEqual(val, self.value)

        for _ in range(2):
            conn, sess = self.open_follower()
            verify(sess)
            sess.close()
            conn.close()

    def test_instantiation_not_globally_visible(self):
        # Reading a deleted page at a timestamp before the truncation forces it to load from disk.
        # The key must be found, cache_read_deleted must increment, and the page must not be dirtied.
        self.setup_leader()
        self.truncate_and_checkpoint(self.trunc_start, self.trunc_stop, 20)
        conn, sess = self.open_follower()

        dirty_before = self.get_stat(conn, stat.conn.cache_pages_dirty)
        rd_before    = self.get_stat(conn, stat.conn.cache_read_deleted)

        # Pre-truncation read forces page load: key found, read_deleted increments, page stays clean.
        ret, val = self.search_at(sess, self.trunc_start + 100, 10)
        self.assertEqual(ret, 0)
        self.assertEqual(val, self.value)
        self.assertGreater(self.get_stat(conn, stat.conn.cache_read_deleted), rd_before)
        self.assertEqual(self.get_stat(conn, stat.conn.cache_pages_dirty), dirty_before)
        self.evict_range(sess, self.trunc_start + 100, self.trunc_start + 100)

        sess.close()
        conn.close()

if __name__ == '__main__':
    wttest.run()
