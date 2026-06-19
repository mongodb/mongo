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

# Regression test. Under preserve_prepared + precise_checkpoint:
#
#   - Commit a value for key 2 and then a remove, so the on-disk image holds
#     a delete with a globally visible stop time.
#   - Open a writer session (session_blocker) that keeps an older transaction
#     id pinned across the prepared work.
#   - Open an eviction session (session_evict) whose snapshot is taken before
#     the prepared transaction starts.
#   - In another session, do a prepared insert on key 2 and roll it back with
#     a rollback_timestamp ahead of stable.
#   - Force eviction via session_evict while stable is still below prepare_ts.
#   - Advance stable past prepare_ts (but below rollback_ts) and force eviction
#     again, then checkpoint.
#
# Expected: subsequent reads see the key as deleted (the rolled-back prepared
# insert leaves the prior committed delete as the visible state), and the
# checkpoint after stable passes prepare_ts writes a preserved prepared cell
# for the still-unresolved aborted prepared marker.

import wiredtiger
import wttest

class test_prepare48(wttest.WiredTigerTestCase):

    test_name = __qualname__
    conn_config = 'precise_checkpoint=true,preserve_prepared=true'
    uri = f'table:{test_name}'

    def force_evict(self, key, read_ts):
        """Force eviction of the page containing `key` via a fresh session."""
        sess = self.conn.open_session()
        try:
            cur = sess.open_cursor(self.uri, None, 'debug=(release_evict)')
            sess.begin_transaction(
                'ignore_prepare=true,read_timestamp=' + self.timestamp_str(read_ts))
            cur.set_key(key)
            self.assertEqual(cur.search(), 0)
            cur.reset()
            cur.close()
            sess.rollback_transaction()
        finally:
            sess.close()

    def assert_not_found(self, key, read_ts):
        sess = self.conn.open_session()
        try:
            cur = sess.open_cursor(self.uri)
            sess.begin_transaction(
                'ignore_prepare=true,read_timestamp=' + self.timestamp_str(read_ts))
            cur.set_key(key)
            self.assertEqual(cur.search(), wiredtiger.WT_NOTFOUND)
            cur.close()
            sess.rollback_transaction()
        finally:
            sess.close()

    def test_aborted_prepared_with_pinned_concurrent_writer(self):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        # Page-companion key kept committed throughout so the page has
        # something for the eviction session cursor to search and release-evict.
        self.session.begin_transaction()
        cursor[1] = 'committed_value'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        # Insert and remove key 2 so the persisted state is a delete.
        self.session.begin_transaction()
        cursor[2] = 'v_init'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        self.session.begin_transaction()
        cursor.set_key(2)
        cursor.remove()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        # Push the [insert, remove] sequence to disk via checkpoint and a
        # forced eviction. Keep stable below the upcoming prepare_timestamp.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(31))
        self.session.checkpoint()
        self.force_evict(1, 20)

        # Advance oldest past the remove so the persisted delete becomes
        # globally visible.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(31))

        # Concurrent writer keeps an active transaction open across the
        # prepared work so its transaction id pins the global oldest active
        # id below the prepared transaction's id.
        session_blocker = self.conn.open_session()
        cursor_blocker = session_blocker.open_cursor(self.uri)
        session_blocker.begin_transaction()
        cursor_blocker[3] = 'blocker_value'
        cursor_blocker.close()

        # The eviction session's transaction begins BEFORE the prepared
        # transaction, so its snapshot does not include the prepared
        # transaction's id when reconciliation runs.
        session_evict = self.conn.open_session()
        session_evict.begin_transaction('ignore_prepare=true')

        # Prepared insert on key 2 (the persisted state is a delete), rolled
        # back with rollback_timestamp ahead of stable.
        session_prep = self.conn.open_session()
        cursor_prep = session_prep.open_cursor(self.uri)
        session_prep.begin_transaction()
        cursor_prep[2] = 'prepared_value'
        session_prep.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(40) +
            ',prepared_id=' + self.prepared_id_str(1))
        cursor_prep.close()
        session_prep.rollback_transaction(
            'rollback_timestamp=' + self.timestamp_str(50))
        session_prep.close()

        # Force eviction via session_evict while stable is still below
        # prepare_timestamp. Reconciliation must preserve the persisted
        # delete so subsequent reads still see "deleted" for key 2.
        evict_cursor = session_evict.open_cursor(
            self.uri, None, 'debug=(release_evict)')
        evict_cursor.set_key(1)
        self.assertEqual(evict_cursor.search(), 0)
        self.assertEqual(evict_cursor.get_value(), 'committed_value')
        evict_cursor.reset()
        evict_cursor.close()
        session_evict.rollback_transaction()
        session_evict.close()

        session_blocker.rollback_transaction()
        session_blocker.close()

        # Stable now passes prepare_timestamp (still below rollback_timestamp).
        # Force another eviction: reconciliation must keep the persisted
        # delete visible (the rolled-back prepared insert leaves the prior
        # committed delete as the visible state). On older builds this
        # eviction aborts because the persisted delete was discarded by the
        # earlier reconcile and the page now has nothing behind the aborted
        # prepared marker.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(42))
        self.force_evict(1, 42)

        # Key 2 is deleted at any timestamp after remove: the prepared
        # insert was rolled back so the visible state reverts to the prior
        # committed delete.
        self.assert_not_found(2, 42)

        # Once rollback_timestamp becomes stable, checkpoint cleans up.
        # Reads still resolve the delete correctly.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(55))
        self.session.checkpoint()
        self.assert_not_found(2, 55)

        cursor.close()
