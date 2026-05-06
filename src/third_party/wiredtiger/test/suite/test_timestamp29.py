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
# test_timestamp29.py
#   Timestamps: Test setting and querying the stable disaggregated schema epoch.

import time
import wiredtiger, wttest
from wiredtiger import stat

# Timestamps: Test setting and querying the stable disaggregated schema epoch.
class test_timestamp29(wttest.WiredTigerTestCase):

    def get_stat(self, stat_name):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        value = stat_cursor[stat_name][2]
        stat_cursor.close()
        return value

    def assertEpochEqual(self, expected_ts):
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=stable_disaggregated_schema_epoch'),
            self.timestamp_str(expected_ts))

    def assertStatEqual(self, stat_name, expected_value, retries=10):
        # Stats may be updated asynchronously, so retry a few times if the expected value is not
        # observed.
        for attempt in range(retries):
            value = self.get_stat(stat_name)
            if value == expected_value:
                return
            if attempt < retries - 1:
                time.sleep(0.1)
        self.assertEqual(value, expected_value)

    def test_base(self):
        '''
        Test setting and querying the stable disaggregated schema epoch, including validation of
        legal and illegal transitions and the corresponding statistics.
        '''
        # When not yet set, querying the epoch returns 0.
        self.assertEpochEqual(0)

        # Set the epoch for the first time.
        self.conn.set_timestamp(
            'stable_disaggregated_schema_epoch=' + self.timestamp_str(10))
        self.assertEpochEqual(10)

        # Stats: one call, one update.
        self.assertStatEqual(stat.conn.txn_set_ts_stable_disagg_epoch, 1)
        self.assertStatEqual(stat.conn.txn_set_ts_stable_disagg_epoch_upd, 1)

        # Advance the epoch forward, which is a legal transition.
        self.conn.set_timestamp(
            'stable_disaggregated_schema_epoch=' + self.timestamp_str(20))
        self.assertEpochEqual(20)

        # Stats: two calls, two updates.
        self.assertStatEqual(stat.conn.txn_set_ts_stable_disagg_epoch, 2)
        self.assertStatEqual(stat.conn.txn_set_ts_stable_disagg_epoch_upd, 2)

        # Setting the epoch to its current value is a no-op (not an error, not an update).
        self.conn.set_timestamp(
            'stable_disaggregated_schema_epoch=' + self.timestamp_str(20))
        self.assertEpochEqual(20)

        # Stats: three calls, still two updates.
        self.assertStatEqual(stat.conn.txn_set_ts_stable_disagg_epoch, 3)
        self.assertStatEqual(stat.conn.txn_set_ts_stable_disagg_epoch_upd, 2)

        # Moving the epoch backwards is illegal.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.set_timestamp(
                'stable_disaggregated_schema_epoch=' + self.timestamp_str(10)),
            '/stable disaggregated schema epoch.*must not be older than current '
            'stable disaggregated schema epoch/')

        # The epoch was not changed by the failed backward transition.
        self.assertEpochEqual(20)

        # Stats: call counter increments even on failed backward attempt; upd counter does not.
        self.assertStatEqual(stat.conn.txn_set_ts_stable_disagg_epoch, 4)
        self.assertStatEqual(stat.conn.txn_set_ts_stable_disagg_epoch_upd, 2)

        # Setting zero is not permitted.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.set_timestamp(
                'stable_disaggregated_schema_epoch=' + self.timestamp_str(0)),
            '/zero not permitted/')

        # The epoch was not changed, but the call stat was still incremented.
        self.assertEpochEqual(20)
        self.assertStatEqual(stat.conn.txn_set_ts_stable_disagg_epoch, 5)
        self.assertStatEqual(stat.conn.txn_set_ts_stable_disagg_epoch_upd, 2)

        # The epoch can be set together with other timestamps in one call.
        self.conn.set_timestamp(
            'oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(40) +
            ',stable_disaggregated_schema_epoch=' + self.timestamp_str(40))
        self.assertEpochEqual(40)
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=stable_timestamp'), self.timestamp_str(40))
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=oldest_timestamp'), self.timestamp_str(10))

        # Stats: six calls, three updates.
        self.assertStatEqual(stat.conn.txn_set_ts_stable_disagg_epoch, 6)
        self.assertStatEqual(stat.conn.txn_set_ts_stable_disagg_epoch_upd, 3)

        # The epoch has no ordering constraint relative to oldest or stable timestamps.
        # It can be set above, equal to, or below either of them.
        self.conn.set_timestamp(
            'stable_disaggregated_schema_epoch=' + self.timestamp_str(50))
        self.assertEpochEqual(50)
        self.conn.set_timestamp(
            'stable_disaggregated_schema_epoch=' + self.timestamp_str(100))
        self.assertEpochEqual(100)

        # Stats: eight calls, five updates.
        self.assertStatEqual(stat.conn.txn_set_ts_stable_disagg_epoch, 8)
        self.assertStatEqual(stat.conn.txn_set_ts_stable_disagg_epoch_upd, 5)
