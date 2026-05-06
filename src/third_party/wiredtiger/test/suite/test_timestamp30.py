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
# test_timestamp30.py
#   Timestamps: Test persisting the stable disaggregated schema epoch in the checkpoint.

import wttest
from helper_disagg import disagg_test_class
from wiredtiger import stat

# Timestamps: Test persisting the stable disaggregated schema epoch in the checkpoint.
@disagg_test_class
class test_timestamp30(wttest.WiredTigerTestCase):

    conn_config = 'statistics=(all),disaggregated=(role="leader",lose_all_my_data=true)'

    def assertEpochEqual(self, expected_ts):
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=stable_disaggregated_schema_epoch'),
            self.timestamp_str(expected_ts))

    def assertLastCheckpointEpochEqual(self, expected_ts):
        self.assertTimestampsEqual(
            self.conn.query_timestamp('get=last_disaggregated_schema_epoch'),
            self.timestamp_str(expected_ts))

    def test_checkpoint(self):
        '''
        Test that the stable disaggregated schema epoch is included in the checkpoint, that it is
        correctly picked up on restart, and that it can be updated in a checkpoint.
        '''
        # Create a layered table so checkpoints write through disaggregated storage.
        uri = 'layered:test_timestamp30'
        self.session.create(uri, 'key_format=S,value_format=S')

        # The first checkpoint, taken before the epoch is set, records 0 in the checkpoint.
        self.session.checkpoint()
        self.assertLastCheckpointEpochEqual(0)

        # Set the timestamps and the epoch, checkpoint, and verify that the epoch value is included
        # in the checkpoint.
        self.conn.set_timestamp(
            'oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(10) +
            ',stable_disaggregated_schema_epoch=' + self.timestamp_str(10))
        self.assertEpochEqual(10)
        self.session.checkpoint()
        self.assertLastCheckpointEpochEqual(10)

        # Restart the connection. The new connection should pick up the latest epoch value from the
        # latest checkpoint.
        with self.expectedStdoutPattern('Removing local file'):
            self.reopen_conn()
        self.assertLastCheckpointEpochEqual(10)

        # Advancing the epoch and the timestamps should update the epoch in the checkpoint to the
        # new value.
        self.conn.set_timestamp(
            'oldest_timestamp=' + self.timestamp_str(20) +
            ',stable_timestamp=' + self.timestamp_str(30) +
            ',stable_disaggregated_schema_epoch=' + self.timestamp_str(30))
        self.assertEpochEqual(30)
        self.session.checkpoint()
        self.assertLastCheckpointEpochEqual(30)

        # A checkpoint without changing the epoch leaves it at the same value.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))
        self.session.checkpoint()
        self.assertLastCheckpointEpochEqual(30)

        # A checkpoint that changes only the epoch also advances the epoch value in the checkpoint.
        self.conn.set_timestamp('stable_disaggregated_schema_epoch=' + self.timestamp_str(40))
        self.session.checkpoint()
        self.assertLastCheckpointEpochEqual(40)

        # Restart the connection to check that the new value was preserved.
        with self.expectedStdoutPattern('Removing local file'):
            self.reopen_conn()
        self.assertEpochEqual(0) # The "current" epoch value must be set explicitly by the caller.
        self.assertLastCheckpointEpochEqual(40)
