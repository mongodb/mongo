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

import threading, time
import wttest
import wiredtiger
from wiredtiger import stat
from wtthread import checkpoint_thread
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_checkpoint28.py
#
# Make sure that when we open a cursor we secure the proper matching
# history store checkpoint, and don't bobble or lose it if the database
# moves on. Non-timestamp version.
#
# It doesn't make sense to run this test for named checkpoints, because
# regenerating a named checkpoint with the cursor open isn't allowed and
# generating two different checkpoints with different names doesn't make
# an interesting scenario. The concern is getting the matching version
# of WiredTigerCheckpoint and hanging onto it.

class test_checkpoint(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all),timing_stress_for_test=[checkpoint_handle]'
    session_config = 'isolation=snapshot'

    format_values = [
        ('column-fix', dict(key_format='r', value_format='8t',
            extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('string_row', dict(key_format='S', value_format='S', extraconfig='')),
    ]
    scenarios = make_scenarios(format_values)

    def large_updates(self, ds, nrows, value):
        cursor = self.session.open_cursor(ds.uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[ds.key(i)] = value
            if i % 101 == 0:
                self.session.commit_transaction()
                self.session.begin_transaction()
        self.session.commit_transaction()
        cursor.close()

    def check(self, cursor, value):
        count = 0
        zero_count = 0
        for k, v in cursor:
            if self.value_format == '8t' and v == 0:
                zero_count += 1
            else:
                self.assertEqual(v, value)
                count += 1
        return count

    def test_checkpoint(self):
        uri_1 = 'table:checkpoint28_1'
        uri_2 = 'table:checkpoint28_2'
        nrows = 1000

        # Create two tables.
        ds_1 = SimpleDataSet(
            self, uri_1, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds_1.populate()

        ds_2 = SimpleDataSet(
            self, uri_2, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds_2.populate()

        if self.value_format == '8t':
            nrows *= 5
            value_a = 97
        else:
            value_a = "aaaaa" * 100

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        # Write more data.
        session2 = self.conn.open_session()
        cursor1 = session2.open_cursor(uri_1)
        cursor2 = session2.open_cursor(uri_2)
        session2.begin_transaction()
        for i in range(1, nrows+1):
            cursor1[ds_1.key(i)] = value_a
            cursor2[ds_2.key(i)] = value_a
        session2.prepare_transaction('prepare_timestamp=' + self.timestamp_str(20))
        
        # Move stable up to 30.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        # Commit the transaction with a background checkpoint so we get part of it
        # in the checkpoint.
        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done)
        try:
            ckpt.start()

            # Wait for checkpoint to start before committing.
            ckpt_started = 0
            while not ckpt_started:
                stat_cursor = self.session.open_cursor('statistics:', None, None)
                ckpt_started = stat_cursor[stat.conn.txn_checkpoint_running][2]
                stat_cursor.close()
                time.sleep(1)

            # Commit the transaction at 25 but make it durable at 35.
            session2.timestamp_transaction('commit_timestamp=' + self.timestamp_str(25))
            session2.timestamp_transaction('durable_timestamp=' + self.timestamp_str(35))
            session2.commit_transaction()
        finally:
            done.set()
            ckpt.join()

        # Open the checkpoint now.
        ckpt_1 = self.session.open_cursor(uri_1, None, 'checkpoint=WiredTigerCheckpoint')
        ckpt_2 = self.session.open_cursor(uri_2, None, 'checkpoint=WiredTigerCheckpoint')

        # Make sure we can't read any of the rows from two tables.
        uri_1_count = self.check(ckpt_1, value_a)
        uri_2_count = self.check(ckpt_2, value_a)
        
        self.assertEqual(uri_1_count, uri_2_count)
        self.assertEqual(uri_1_count, 0)

        ckpt_1.close()
        ckpt_2.close()

if __name__ == '__main__':
    wttest.run()
