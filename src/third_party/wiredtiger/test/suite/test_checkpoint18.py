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

# test_checkpoint18.py
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
    conn_config = 'statistics=(all),timing_stress_for_test=[checkpoint_slow]'
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

    def do_checkpoint(self, ckpt_name):
        if ckpt_name is None:
            self.session.checkpoint()
        else:
            self.session.checkpoint('name=' + ckpt_name)

    def check(self, cursor, nrows, value):
        #self.session.begin_transaction()
        count = 0
        for k, v in cursor:
            self.assertEqual(v, value)
            count += 1
        self.assertEqual(count, nrows)
        #self.session.rollback_transaction()

    def test_checkpoint(self):
        uri = 'table:checkpoint18'
        nrows = 10000

        # Create a table.
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()

        if self.value_format == '8t':
            nrows *= 5
            value_a = 97
            value_b = 98
            value_c = 99
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100
            value_c = "ccccc" * 100

        # Write some baseline data and checkpoint it.
        self.large_updates(ds, nrows, value_a)
        self.session.checkpoint()

        # Write more data. Touch odd keys only. Hold the transaction open.
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(uri)
        session2.begin_transaction()
        for i in range(1, nrows + 1, 2):
            cursor2[ds.key(i)] = value_b

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

            session2.commit_transaction()
        finally:
            done.set()
            ckpt.join()

        # Now write the other keys.
        session2.begin_transaction()
        for i in range(2, nrows + 2, 2):
            cursor2[ds.key(i)] = value_c
        session2.commit_transaction()

        # Open the checkpoint now.
        ckpt = self.session.open_cursor(uri, None, 'checkpoint=WiredTigerCheckpoint')

        # Take another checkpoint. This will write out the rest of the partial
        # transaction and remove its history store footprint, so reading from the
        # first checkpoint will fail if the cursor hasn't secured itself a reference
        # to its matching history store checkpoint.
        self.session.checkpoint()

        # Make sure we can read the table from the second checkpoint.
        # We shouldn't see either value_b or value_c.
        self.check(ckpt, nrows, value_a)
        ckpt.close()

        # Note that it would be nice to crosscheck that the first checkpoint was in fact
        # inconsistent. Could do that by copying the database before the second checkpoint
        # and opening the copy here, I guess. FUTURE?

if __name__ == '__main__':
    wttest.run()
