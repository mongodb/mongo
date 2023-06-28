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
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_checkpoint19.py
#
# Make sure that when we open a cursor we secure the proper matching
# history store checkpoint, and don't bobble or lose it if the database
# moves on. Timestamped version.
#
# It doesn't make sense to run this test for named checkpoints, because
# regenerating a named checkpoint with the cursor open isn't allowed and
# generating two different checkpoints with different names doesn't make
# an interesting scenario. The concern is getting the matching version
# of WiredTigerCheckpoint and hanging onto it.

class test_checkpoint(wttest.WiredTigerTestCase):
    session_config = 'isolation=snapshot'

    format_values = [
        ('column-fix', dict(key_format='r', value_format='8t',
            extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('string_row', dict(key_format='S', value_format='S', extraconfig='')),
    ]
    scenarios = make_scenarios(format_values)

    def large_updates(self, ds, nrows, value, ts):
        cursor = self.session.open_cursor(ds.uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[ds.key(i)] = value
            if i % 101 == 0:
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
                self.session.begin_transaction()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()

    def do_checkpoint(self, ckpt_name):
        if ckpt_name is None:
            self.session.checkpoint()
        else:
            self.session.checkpoint('name=' + ckpt_name)

    def check(self, cursor, nrows, oddvalue, evenvalue):
        #self.session.begin_transaction()
        count = 0
        for k, v in cursor:
            # Alas, count is even when the key number is odd.
            if count % 2 == 0:
                self.assertEqual(v, oddvalue)
            else:
                self.assertEqual(v, evenvalue)
            count += 1
        self.assertEqual(count, nrows)
        #self.session.rollback_transaction()

    def test_checkpoint(self):
        uri = 'table:checkpoint17'
        nrows = 1000

        # Create a table.
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
            value_d = 100
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100
            value_c = "ccccc" * 100
            value_d = "ddddd" * 100

        # Set oldest and stable to 5.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(5) +
            ',stable_timestamp=' + self.timestamp_str(5))

        # Write some baseline data and checkpoint it.
        self.large_updates(ds, nrows, value_a, 10)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        # Write more data. Touch odd keys only. Do two rounds to make sure there's history.
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1, 2):
            cursor[ds.key(i)] = value_b
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        self.session.begin_transaction()
        for i in range(1, nrows + 1, 2):
            cursor[ds.key(i)] = value_c
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        # Now checkpoint.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        self.session.checkpoint()

        # Now write the other keys.
        self.session.begin_transaction()
        for i in range(2, nrows + 2, 2):
            cursor[ds.key(i)] = value_d
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(40))

        # Open the checkpoint now.
        # Since we can't change the cursor read timestamp on the fly, get a separate one
        # for each read time we're interested in.
        def cfgstr(ts):
            return 'checkpoint=WiredTigerCheckpoint,debug=(checkpoint_read_timestamp={})'.format(
                self.timestamp_str(ts))
        ckpt10 = self.session.open_cursor(uri, None, cfgstr(10))
        ckpt20 = self.session.open_cursor(uri, None, cfgstr(20))
        ckpt30 = self.session.open_cursor(uri, None, cfgstr(30))

        # Take another checkpoint. Advance oldest so this checkpoint will throw away
        # some of the history. (Because we've interleaved the keys, we're guaranteed
        # that every history store page involved will be rewritten.) Reading from the
        # first checkpoint will fail if the cursor hasn't secured itself a reference
        # to its matching history store checkpoint.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(35))
        self.session.checkpoint()

        # Make sure we can still read the history.
        self.check(ckpt10, nrows, value_a, value_a)
        self.check(ckpt20, nrows, value_b, value_a)
        self.check(ckpt30, nrows, value_c, value_a)

        ckpt10.close()
        ckpt20.close()
        ckpt30.close()

if __name__ == '__main__':
    wttest.run()
