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
from prepare_util import test_prepare_preserve_prepare_base

# Tests checkpoint behavior with aborted prepared transactions based on stable timestamp:
# - Skip writing aborted prepared updates when rollback timestamp is stable
# - Skip writing when prepare timestamp is not stable
# - Write prepared updates when prepare timestamp is stable but rollback timestamp is not

class test_prepare31(test_prepare_preserve_prepare_base):
    uri = 'table:test_prepare31'

    def setup_initial_data(self, uri):
        """Set up initial test data and verify it's accessible."""
        # Set initial timestamps - start with lower values
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

        create_params = 'key_format=i,value_format=S'
        self.session.create(uri, create_params)

        # Insert some initial data that will be committed
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 100):
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
        cursor.set_key(50)
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), "initial_value_50")
        self.session.commit_transaction()
        cursor.close()

    def create_prepared_transaction(self, uri, prepare_ts, rollback_ts):
        """Create a prepared transaction and roll it back."""
        session_prepare = self.conn.open_session()
        cursor_prepare = session_prepare.open_cursor(self.uri)
        session_prepare.begin_transaction()

        # Make updates in the prepared transaction
        for i in range(1, 100):
            cursor_prepare[i] = "prepared_value_" + str(i)

        session_prepare.prepare_transaction('prepare_timestamp=' + self.timestamp_str(prepare_ts) + ',prepared_id=' + self.prepared_id_str(1))
        session_prepare.rollback_transaction('rollback_timestamp=' + self.timestamp_str(rollback_ts))

        cursor_prepare.close()
        session_prepare.close()

    def check_prepared_stat(self, expected_value):
        """Check the rec_time_window_prepared statistic."""
        stat_cursor = self.session.open_cursor('statistics:')
        rec_time_window_prepared = stat_cursor[wiredtiger.stat.dsrc.rec_time_window_prepared][2]
        self.assertEqual(rec_time_window_prepared, expected_value)
        stat_cursor.close()

    @wttest.skip_for_hook("disagg", "Skip test until cell packing/unpacking is supported for page delta")
    def test_skip_aborted_prepare_update_if_stable_rollback_timestamp(self):
        self.setup_initial_data(self.uri)

        # Create and rollback a prepared transaction
        self.create_prepared_transaction(self.uri, prepare_ts=70, rollback_ts=80)

        # This makes the rollback timestamp "stable"
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(90))

        # Force checkpoint to write data to disk - this should skip the aborted prepared updates
        # since their rollback timestamp (80) is less than stable timestamp (90)
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
        }, self.uri)

    @wttest.skip_for_hook("disagg", "Skip test until cell packing/unpacking is supported for page delta")
    def test_skip_aborted_prepare_update_if_prepare_timestamp_not_stable(self):
        self.setup_initial_data(self.uri)

        # Create and rollback a prepared transaction
        self.create_prepared_transaction(self.uri, prepare_ts=70, rollback_ts=80)

        # Force checkpoint to write data to disk - this should skip the aborted prepared updates
        # since their prepare timestamp is after stable timestamp
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
        }, self.uri)

    @wttest.skip_for_hook("disagg", "Skip test until cell packing/unpacking is supported for page delta")
    def test_write_prepare_update_if_rollback_timestamp_not_stable(self):
        self.setup_initial_data(self.uri)

        # Create and rollback a prepared transaction
        self.create_prepared_transaction(self.uri, prepare_ts=70, rollback_ts=80)

        # Set table timestamp to be after prepare timestamp, but before rollback timestamp.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(75))

        # Since prepare timestamp is stable but rollback ts is not, we write the prepared update to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: True,
        }, self.uri)
