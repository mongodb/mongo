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

# test_prepare46.py
# Regression test: a prepare rollback whose prepare timestamp is not yet stable
# at the time of eviction should leave the prepared cell available for the next
# checkpoint. Once the prepare timestamp becomes stable, the checkpoint must
# write the preserved prepared cell to disk, not a tombstone.
#
# Update chain for key 2 (fresh insert, no prior committed value):
#   [rollback tombstone] -> [aborted prepare (prepare_ts=30, rollback_ts=50)]
#
# Step 1: eviction at stable=25 (prepare_ts=30 not yet stable):
#   Reconciliation must not write the rollback tombstone because the rollback
#   decision is not yet stable. The page stays dirty for a later reconcile.
#
#   Without the fix, the tombstone is incorrectly selected and marked as already
#   written to disk, poisoning subsequent reconciliations.
#
# Step 2: checkpoint at stable=35 (prepare_ts=30 now stable, rollback_ts=50 not):
#   Reconciliation should write the preserved prepared cell to disk.
#
#   Without the fix, the poisoned tombstone is re-selected and the prepared cell
#   is never written.
#   With the fix, step 1 correctly defers the tombstone and step 2 writes the
#   prepared cell.

import wiredtiger
import wttest

class test_prepare46(wttest.WiredTigerTestCase):

    conn_config = 'precise_checkpoint=true,preserve_prepared=true'
    uri = 'table:test_prepare46'

    def test_prepared_cell_preserved_after_eviction_at_unstable_prepare_ts(self):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        # Commit a value for key 1 at ts=20 so the page has something to evict.
        self.session.begin_transaction()
        cursor[1] = 'committed_value'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        # stable=25 is intentionally below prepare_ts=30 so that the prepare is
        # not yet stable when eviction runs.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(25))

        # session_blocker holds an active write transaction started before
        # session_prep and kept open through eviction. Its presence as a
        # concurrent writer is what places reconciliation on the code path where
        # the tombstone must be deferred, the exact path where the bug manifests.
        session_blocker = self.conn.open_session()
        cursor_blocker = session_blocker.open_cursor(self.uri)
        session_blocker.begin_transaction()
        cursor_blocker[3] = 'blocker_value'
        cursor_blocker.close()

        session_evict = self.conn.open_session()
        session_evict.begin_transaction('ignore_prepare=true')

        # Fresh insert on key 2 at prepare_ts=30, rolled back at rollback_ts=50.
        # Because key 2 has no prior committed value the rollback prepends a
        # globally visible tombstone:
        #   [rollback tombstone] -> [aborted prepare (rollback_ts=50)]
        session_prep = self.conn.open_session()
        cursor_prep = session_prep.open_cursor(self.uri)
        session_prep.begin_transaction()
        cursor_prep[2] = 'prepared_value'
        session_prep.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(30) +
            ',prepared_id=' + self.prepared_id_str(1))
        cursor_prep.close()
        session_prep.rollback_transaction('rollback_timestamp=' + self.timestamp_str(50))
        session_prep.close()

        # Evict at stable=25: prepare_ts=30 is not yet stable.
        # Without the fix the tombstone is incorrectly written to disk,
        # preventing the prepared cell from being written at the next checkpoint.
        evict_cursor = session_evict.open_cursor(self.uri, None, 'debug=(release_evict)')
        evict_cursor.set_key(1)
        self.assertEqual(evict_cursor.search(), 0)
        self.assertEqual(evict_cursor.get_value(), 'committed_value')
        evict_cursor.reset()
        evict_cursor.close()
        session_evict.rollback_transaction()
        session_evict.close()

        session_blocker.rollback_transaction()
        session_blocker.close()

        # stable=35: prepare_ts=30 is now stable but rollback_ts=50 is not.
        # The checkpoint must write the preserved prepared cell to disk.
        # Without the fix the tombstone is re-selected and no prepared cell is written.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(35))
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: True,
        }, self.uri)

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(20))
        cursor.set_key(1)
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), 'committed_value')
        self.session.rollback_transaction()

        # stable=55: rollback_ts=50 is now stable; the tombstone should be written
        # cleanly with no preserved prepared cell.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(55))
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
        }, self.uri)

        # After the tombstone is written, reads at ts=20 must still resolve via
        # the history store.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(20))
        cursor.set_key(1)
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), 'committed_value')
        self.session.rollback_transaction()

        cursor.close()
