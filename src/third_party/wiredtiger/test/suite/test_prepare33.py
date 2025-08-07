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

# Tests checkpoint behavior for rolled back prepared transactions with tombstones:
# - Write aborted prepared tombstones as prepared if prepared timestamp is stable but rollback timestamp is not stable
# - Skip writing aborted prepared tobstone if rollback timestamp is stable

class test_prepare33(wttest.WiredTigerTestCase):

    conn_config = 'checkpoint=(precise=true),preserve_prepared=true,statistics=(all)'
    uri = 'table:test_prepare32'

    def get_stats(self, stats):
        """Get the current values of multiple statistics."""
        stat_cursor = self.session.open_cursor('statistics:'+self.uri)
        results = {}
        for stat in stats:
            results[stat] = stat_cursor[stat][2]
        stat_cursor.close()
        return results

    def checkpoint_and_verify_stats(self, expected_changes):
        """
        Perform a checkpoint and verify the expected changes in multiple statistics.

        Args:
            expected_changes: Dict mapping stat -> bool
                             where True means expect increase, False means expect no change
        """
        stats_to_check = list(expected_changes.keys())
        old_stats = self.get_stats(stats_to_check)

        self.session.checkpoint()

        new_stats = self.get_stats(stats_to_check)

        for stat, expect_increase in expected_changes.items():
            diff = new_stats[stat] - old_stats[stat]
            if expect_increase:
                self.assertGreater(diff, 0,
                    f"Stat {stat}: expected increase, got diff {diff}")
            else:
                self.assertEqual(diff, 0,
                    f"Stat {stat}: expected no change, got diff {diff}")

        return new_stats

    def test_rollbacked_prepare(self):
        # Set initial timestamps - start with lower values
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        if 'disagg' in self.hook_names:
            self.skipTest("Skip test until cell packing/unpacking is supported for page delta and tier storage")
        create_params = 'key_format=i,value_format=S'
        self.session.create(self.uri, create_params)

        # Insert some initial data that will be committed
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 10):
            cursor.set_key(i)
            cursor.set_value("initial_value_" + str(i))
            cursor.insert()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor.close()

        # Advance stable timestamp after the commit
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))

        # Verify initial data is there
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(35))
        cursor.set_key(1)
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), "initial_value_1")
        self.session.commit_transaction()
        cursor.close()

        # Checkpoint should write initial data
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
        })
        # Start a prepared transaction that will be committed
        session_prepare = self.conn.open_session()
        cursor_prepare = session_prepare.open_cursor(self.uri)
        session_prepare.begin_transaction()

        # Make updates in the prepared transaction
        for i in range(1, 100):
            cursor_prepare[i] = "prepared_value_" + str(i)
            cursor_prepare.set_key(1)
            cursor_prepare.remove()
        cursor_prepare.close()
        # Prepare the transaction with timestamp 70
        session_prepare.prepare_transaction('prepare_timestamp=' + self.timestamp_str(70)+',prepared_id=' + self.prepared_id_str(1))
        # Commit the transaction at timestamp 80
        session_prepare.rollback_transaction("rollback_timestamp=" + self.timestamp_str(80))
        session_prepare.close()
        # Checkpoint, current stable timestamp is 40 so prepare timestamp is not stable
        # Should not write prepare.
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
        })

        # Move stable timestamp to after prepared timestamp, but before durable timestamp
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(75))
        # Should write update as prepared, write both start and stop prepare
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: True,
            wiredtiger.stat.dsrc.rec_time_window_start_txn: True,
            wiredtiger.stat.dsrc.rec_time_window_stop_txn: True,
        })

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(85))
        # Skip writing aborted prepared update because its rollback timestamp is stable
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_durable_start_ts: True,
            wiredtiger.stat.dsrc.rec_time_window_start_ts: True,
            wiredtiger.stat.dsrc.rec_time_window_start_txn: True,
            wiredtiger.stat.dsrc.rec_time_window_durable_stop_ts: False,
            wiredtiger.stat.dsrc.rec_time_window_stop_ts: False,
            wiredtiger.stat.dsrc.rec_time_window_stop_txn: False,
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
        })
