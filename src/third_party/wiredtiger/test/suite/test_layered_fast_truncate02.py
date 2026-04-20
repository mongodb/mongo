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

# test_layered_fast_truncate02.py
#   Validates visibility and cursor behavior when a follower picks up a
#   checkpoint containing fast-truncated pages.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_fast_truncate02(wttest.WiredTigerTestCase):

    uri         = 'layered:test_layered_fast_truncate02'
    nrows       = 5000
    value       = 'a' * 500
    trunc_start = 1001
    trunc_stop  = 4000
    trunc_mid   = (trunc_start + trunc_stop) // 2

    conn_config = 'cache_size=50MB,statistics=(all),disaggregated=(role="leader")'
    disagg_storages = gen_disagg_storages('test_layered_fast_truncate02', disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def setUp(self):
        if wiredtiger.disagg_fast_truncate_build() == 0:
            self.skipTest("fast truncate support is not enabled")
        super().setUp()

    def leader_checkpoint(self, ts):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(ts) +
                                ',oldest_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()

    def setup_leader(self):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        self.session.create(self.uri, 'key_format=i,value_format=S')
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

    def test_visibility(self):
        # At ts=20 (equal to truncation at ts=20): truncated keys return WT_NOTFOUND, boundary and
        # exterior keys return their values. At ts=15 (before truncation): all keys are visible.
        self.setup_leader()
        self.truncate_and_checkpoint(self.trunc_start, self.trunc_stop, 20)
        conn, sess = self.open_follower()

        # Truncation is visible: deleted keys are gone, surrounding keys survive.
        for key in [self.trunc_start, self.trunc_mid, self.trunc_stop]:
            ret, _ = self.search_at(sess, key, 20)
            self.assertEqual(ret, wiredtiger.WT_NOTFOUND)

        for key in [1, self.trunc_start - 1, self.trunc_stop + 1, self.nrows]:
            ret, val = self.search_at(sess, key, 20)
            self.assertEqual(ret, 0)
            self.assertEqual(val, self.value)

        # Truncation is not visible: all keys exist at a timestamp before the truncation.
        for key in [self.trunc_start, self.trunc_mid, self.trunc_stop]:
            ret, val = self.search_at(sess, key, 15)
            self.assertEqual(ret, 0)
            self.assertEqual(val, self.value)

        sess.close()
        conn.close()

    def test_pre_truncation_read_sees_all_rows(self):
        # Reading at a timestamp before the truncation must still find all rows, including those
        # later deleted. Verifies mvcc correctness across the follower checkpoint boundary.
        self.setup_leader()
        self.truncate_and_checkpoint(self.trunc_start, self.trunc_stop, 20)
        conn, sess = self.open_follower()

        for key in [self.trunc_start, self.trunc_mid, self.trunc_stop]:
            ret, val = self.search_at(sess, key, 10)
            self.assertEqual(ret, 0)
            self.assertEqual(val, self.value)

        cur = sess.open_cursor(self.uri)
        sess.begin_transaction('read_timestamp=' + self.timestamp_str(10))
        count = 0
        while cur.next() == 0:
            count += 1
        sess.rollback_transaction()
        cur.close()
        self.assertEqual(count, self.nrows)

        sess.close()
        conn.close()

    def test_cursor_scanning(self):
        # Forward and backward scans must skip the entire truncated range without visiting any
        # deleted key. search_near on a deleted key must land outside the range.
        self.setup_leader()
        self.truncate_and_checkpoint(self.trunc_start, self.trunc_stop, 20)
        conn, sess = self.open_follower()

        expected    = self.nrows - (self.trunc_stop - self.trunc_start + 1)
        trunc_range = range(self.trunc_start, self.trunc_stop + 1)

        # Forward scan: verify no deleted key is visited and the gap is jumped correctly.
        cur = sess.open_cursor(self.uri)
        sess.begin_transaction('read_timestamp=' + self.timestamp_str(25))
        count, prev_key, first_after_gap = 0, 0, None
        while cur.next() == 0:
            key = cur.get_key()
            self.assertNotIn(key, trunc_range)
            if prev_key == self.trunc_start - 1 and first_after_gap is None:
                first_after_gap = key
            prev_key = key
            count += 1
        sess.rollback_transaction()
        cur.close()
        self.assertEqual(count, expected)
        self.assertEqual(first_after_gap, self.trunc_stop + 1)

        # Backward scan: same row count, gap jumped in reverse.
        cur = sess.open_cursor(self.uri)
        sess.begin_transaction('read_timestamp=' + self.timestamp_str(25))
        count, prev_key, first_before_gap = 0, self.nrows + 1, None
        while cur.prev() == 0:
            key = cur.get_key()
            self.assertNotIn(key, trunc_range)
            if prev_key == self.trunc_stop + 1 and first_before_gap is None:
                first_before_gap = key
            prev_key = key
            count += 1
        sess.rollback_transaction()
        cur.close()
        self.assertEqual(count, expected)
        self.assertEqual(first_before_gap, self.trunc_start - 1)

        # search_near on a deleted key must land on the nearest live boundary key.
        cur = sess.open_cursor(self.uri)
        sess.begin_transaction('read_timestamp=' + self.timestamp_str(25))
        cur.set_key(self.trunc_mid)
        cmp = cur.search_near()
        self.assertIn(cmp, (-1, 1))
        landed = cur.get_key()
        self.assertNotIn(landed, trunc_range)
        self.assertEqual(landed, self.trunc_start - 1 if cmp == -1 else self.trunc_stop + 1)
        sess.rollback_transaction()
        cur.close()

        sess.close()
        conn.close()

if __name__ == '__main__':
    wttest.run()
