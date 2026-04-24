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
#
# test_cursor26.py
#   Version cursor with show_prepared_rollback=true must emit every
#   version of a key that a caller would need to reconstruct its
#   history: both rolled-back prepared values and any underlying
#   committed value that persists after the rollback. Regression for
#   WT-17240, where an underlying committed value was dropped when it
#   had been reconciled to disk before the prepared rollback.
#

import wttest

WT_TXN_ABORTED = 2**64 - 1

class test_cursor26(wttest.WiredTigerTestCase):
    uri = 'file:test_cursor26.wt'

    conn_config = (
        'cache_size=50MB,statistics=(all),'
        'preserve_prepared=true,precise_checkpoint=true'
    )

    # Field indices in the version cursor's value tuple.
    F_START_TXN = 0
    F_START_TS = 1
    F_TYPE = 10
    F_VALUE = 14

    WT_UPDATE_STANDARD = 3

    def create_table(self):
        self.session.create(self.uri,
            'key_format=i,value_format=i,in_memory=true,log=(enabled=false)')

    def open_version_cursor(self):
        """Version cursor configured the way disagg drain opens it."""
        config = (
            "debug=(dump_version=(enabled=true,raw_key_value=true,visible_only=true,"
            "timestamp_order=true,cross_key=true,show_prepared_rollback=true))"
        )
        return self.session.open_cursor(self.uri, None, config)

    def all_versions(self, key):
        """Collect every version the cursor emits for key, in emission order."""
        self.session.begin_transaction()
        vc = self.open_version_cursor()
        try:
            vc.set_key(key)
            if vc.search() != 0:
                return []
            rows = [vc.get_values()]
            while vc.next() == 0:
                if vc.get_key() != key:
                    break
                rows.append(vc.get_values())
            return rows
        finally:
            vc.close()
            self.session.rollback_transaction()

    def force_reconcile(self, key):
        """Evict the page holding key so its in-memory chain is reconciled."""
        sess = self.conn.open_session("debug=(release_evict_page=true)")
        sess.begin_transaction("ignore_prepare=true")
        c = sess.open_cursor(self.uri, None)
        c.set_key(key)
        c.search()
        c.reset()
        c.close()
        sess.rollback_transaction()
        sess.close()

    def commit_put(self, key, value, ts):
        c = self.session.open_cursor(self.uri, None)
        self.session.begin_transaction()
        c[key] = value
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        c.close()

    def prepared_put_and_rollback(self, key, value, prepare_ts, rollback_ts, prepared_id):
        sess = self.conn.open_session()
        c = sess.open_cursor(self.uri, None)
        sess.begin_transaction()
        c[key] = value
        # Force the prepared update to be visible on disk before the rollback.
        sess.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(prepare_ts) +
            ',prepared_id=' + self.prepared_id_str(prepared_id))
        self.force_reconcile(key)
        sess.rollback_transaction(
            'rollback_timestamp=' + self.timestamp_str(rollback_ts))
        c.close()
        sess.close()

    def test_aborted_prepared_does_not_hide_underlying_committed(self):
        """
        A key has a committed value and is then targeted by a prepared
        update that is rolled back. After rollback, a version cursor
        opened with show_prepared_rollback=true must emit two rows for
        the key: the rolled-back prepared value and the underlying
        committed value.
        """
        self.create_table()

        # Step 1: establish a persistent committed value for the key.
        self.commit_put(1, 10, 10)

        # Step 2: prepared update targeting the same key, rolled back.
        # Reconcile happens while the prepared update is active so the
        # persistent value has been written to disk before rollback.
        self.prepared_put_and_rollback(
            key=1, value=20, prepare_ts=20, rollback_ts=30, prepared_id=1)

        # Allow the connection to close cleanly with precise_checkpoint.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        rows = self.all_versions(1)

        # Two versions expected: the rolled-back prepared value and the
        # underlying committed value. Dropping either leaves downstream
        # consumers (e.g. the disagg drain) without enough information to
        # reconstruct the key's history.
        self.assertEqual(len(rows), 2,
            "expected 2 versions for key 1, got {}: {}".format(len(rows), rows))

        rolled_back, committed = rows

        # Rolled-back prepared row: identified by start_txn == WT_TXN_ABORTED
        # and carries the prepared value that never actually landed.
        self.assertEqual(rolled_back[self.F_START_TXN], WT_TXN_ABORTED,
            "first row should be the rolled-back prepared value")
        self.assertEqual(rolled_back[self.F_VALUE], 20)

        # Committed row: the value that remains after the rollback.
        self.assertNotEqual(committed[self.F_START_TXN], WT_TXN_ABORTED,
            "second row should be the surviving committed value")
        self.assertEqual(committed[self.F_START_TS], 10)
        self.assertEqual(committed[self.F_TYPE], self.WT_UPDATE_STANDARD)
        self.assertEqual(committed[self.F_VALUE], 10)
