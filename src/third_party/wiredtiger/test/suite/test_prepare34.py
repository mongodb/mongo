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

# Tests checkpoint behavior for prepared modify operations:
# - Stable prepares written as prepared
# - Committed/rolled back properly handled
# FIXME: Verify that prepared modifies are reconstructed properly when loaded from disk

class test_prepare34(test_prepare_preserve_prepare_base):
    uri = 'table:test_prepare34'

    def test_rollback_prepare_modify(self):
        """
        Test that prepared transactions containing modify operations that are rolled back
        do not affect checkpoint behavior or data reconstruction.
        """
        value = 'aaaaa'
        # Set initial timestamps
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

        uri = 'table:test_prepare34'
        create_params = 'key_format=i,value_format=S'
        self.session.create(self.uri, create_params)

        # Insert baseline data that will remain unaffected
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 21):  # Keys 1-20
            cursor.set_key(i)
            cursor.set_value(value)
            cursor.insert()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        # cursor.close()

        # Advance stable timestamp after baseline commit
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))

        # Initial checkpoint should write baseline data, no prepared content
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
        }, self.uri)

        # Create a prepared transaction with modify operations
        session_prepare = self.conn.open_session()
        cursor_prepare = session_prepare.open_cursor(self.uri)
        session_prepare.begin_transaction()

        for i in range(1, 21):
            cursor_prepare.set_key(i)
            long_b_string = 'b' * 65  # String of 65 'b' characters
            modifications = [wiredtiger.Modify(long_b_string, 0, 65)]  # Modify 'aaaaa' to include 65 'b' characters at start
            self.assertEqual(cursor_prepare.modify(modifications), 0)

        for i in range(1, 21):
            cursor_prepare.set_key(i)
            long_d_string = 'd' * 70  # String of 70 'd' characters
            modifications = [wiredtiger.Modify(long_d_string, 0, 70)]  # Modify to include 70 'd' characters at start
            self.assertEqual(cursor_prepare.modify(modifications), 0)
        # Prepare the transaction at timestamp 70
        session_prepare.prepare_transaction('prepare_timestamp=' + self.timestamp_str(70)+',prepared_id=' + self.prepared_id_str(1))

        # Checkpoint while transaction is prepared but stable timestamp is before prepare timestamp
        # Should not write any prepared content since stable timestamp (40) < prepare timestamp (70)
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
        }, self.uri)

        # Now rollback the prepared transaction at timestamp 80
        session_prepare.rollback_transaction('rollback_timestamp='+ self.timestamp_str(80))

        # Move stable timestamp to after prepare timestamp but before committing
        # Should write prepared modifies to disk
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(75))
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: True,
        }, self.uri)

        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(75)+ ',isolation=read-uncommitted')
        for i in range(1, 21):
            self.assertEqual(value, cursor[i])
        self.session.rollback_transaction()

        # Write aborted update to disk when rollback ts is stable
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(80))
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
            wiredtiger.stat.dsrc.rec_time_window_stop_ts: False,
            wiredtiger.stat.dsrc.rec_time_window_start_ts: True,
        }, self.uri)

        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(75))
        for i in range(1, 21):
            self.assertEqual(value, cursor[i])
        self.session.rollback_transaction()

    def test_commit_prepare_modify(self):
        """
        Test that prepared transactions containing modify operations that are rolled back
        do not affect checkpoint behavior or data reconstruction.
        """
        value = 'aaaaa'
        long_b_string = 'b' * 65  # String of 65 'b' characters
        long_d_string = 'd' * 70  # String of 70 'd' characters
        # Set initial timestamps
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

        create_params = 'key_format=i,value_format=S'
        self.session.create(self.uri, create_params)

        # Insert baseline data that will remain unaffected
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 21):  # Keys 1-20
            cursor.set_key(i)
            self.session.breakpoint()
            cursor.set_value(value)
            cursor.insert()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        # Advance stable timestamp after baseline commit
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))

        # Initial checkpoint should write baseline data, no prepared content
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
        }, self.uri)

        # Create a prepared transaction with modify operations
        session_prepare = self.conn.open_session()
        cursor_prepare = session_prepare.open_cursor(self.uri)
        session_prepare.begin_transaction()

        for i in range(1, 21):
            cursor_prepare.set_key(i)
            modifications = [wiredtiger.Modify(long_b_string, 0, 0)]  # Modify 'aaaaa' to include 65 'b' characters at start
            self.assertEqual(cursor_prepare.modify(modifications), 0)

        for i in range(1, 21):
            cursor_prepare.set_key(i)

            modifications = [wiredtiger.Modify(long_d_string, 0, 0)]  # Modify to include 70 'd' characters at start
            self.assertEqual(cursor_prepare.modify(modifications), 0)

        # Prepare the transaction at timestamp 70
        session_prepare.prepare_transaction('prepare_timestamp=' + self.timestamp_str(70)+',prepared_id=' + self.prepared_id_str(1))

        # Checkpoint while transaction is prepared but stable timestamp is before prepare timestamp
        # Should not write any prepared content since stable timestamp (40) < prepare timestamp (70)
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
        }, self.uri)

        # Now commit the prepared transaction at timestamp 80
        session_prepare.commit_transaction('commit_timestamp='+ self.timestamp_str(80)+',durable_timestamp='+self.timestamp_str(90))

        # Move stable timestamp to after prepare timestamp but before committing
        # Should write prepared modifies to disk
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(75))
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: True,
        }, self.uri)

        # Write prepare update to disk when prepare ts is stable but durable timestamp is not stable
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(80))
        if 'disagg' in self.hook_names:
            # We should write an empty delta as we still write prepared.
            self.checkpoint_and_verify_stats({
                wiredtiger.stat.dsrc.rec_page_delta_leaf: False,
            }, self.uri)
        else:
            self.checkpoint_and_verify_stats({
                wiredtiger.stat.dsrc.rec_time_window_prepared: True,
            }, self.uri)

        # Write committed update to disk when prepare ts is stable but durable timestamp is not stable
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(95))
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_durable_start_ts: True,
            wiredtiger.stat.dsrc.rec_time_window_start_ts: True,
            wiredtiger.stat.dsrc.rec_time_window_start_txn: True,
            wiredtiger.stat.dsrc.rec_time_window_durable_stop_ts: False,
            wiredtiger.stat.dsrc.rec_time_window_stop_ts: False,
            wiredtiger.stat.dsrc.rec_time_window_stop_txn: False,
            wiredtiger.stat.dsrc.rec_time_window_prepared: False,
        }, self.uri)

        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(75))
        for i in range(1, 21):
            self.assertEqual(value, cursor[i])
        self.session.rollback_transaction()

        self.session.begin_transaction('read_timestamp='+ self.timestamp_str(81))
        for i in range(1, 21):
            expected_value = long_d_string + long_b_string + 'aaaaa'
            self.assertEqual(expected_value, cursor[i])
        self.session.rollback_transaction()
