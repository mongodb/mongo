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
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_checkpoint12.py
# Make sure you can't read from a checkpoint while you have a prepared transaction.
# (This is to make sure that any transaction shenanigans involved in reading from
# checkpoints don't interfere with the blanket ban on doing other operations after
# preparing.)

class test_checkpoint(wttest.WiredTigerTestCase):
    conn_config = ''
    session_config = 'isolation=snapshot'

    operation_values = [
        ('search', dict(op='search')),
        ('next', dict(op='next')),
        ('prev', dict(op='prev')),
        ('search_near', dict(op='search_near')),
    ]
    scenarios = make_scenarios(operation_values)

    # No need to run this on more than one btree type.
    key_format = 'r'
    value_format = 'S'

    def large_updates(self, uri, ds, nrows, value, ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[ds.key(i)] = value
            if i % 101 == 0:
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
                self.session.begin_transaction()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()

    def operate(self, ckpt_cursor):
        if self.op == 'search':
            ckpt_cursor.search()
        elif self.op == 'next':
            ckpt_cursor.next()
        elif self.op == 'prev':
            ckpt_cursor.prev()
        elif self.op == 'search_near':
            ckpt_cursor.search_near()
        else:
            self.assertTrue(False)

    def test_checkpoint(self):
        uri = 'table:checkpoint12'
        nrows = 1000

        # Create a table.
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100
            value_c = "ccccc" * 100

        # Pin oldest and stable timestamps to 5.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(5) +
            ',stable_timestamp=' + self.timestamp_str(5))

        # Write some data at time 10.
        self.large_updates(uri, ds, nrows, value_a, 10)

        # Make a checkpoint.
        self.session.checkpoint()

        # Write some more data at time 20.
        self.large_updates(uri, ds, nrows, value_a, 20)

        # Open the checkpoint.
        ckpt_cursor = self.session.open_cursor(uri, None, 'checkpoint=WiredTigerCheckpoint')
        ckpt_cursor.set_key(ds.key(1))

        # Write some further data, and prepare it at time 30.
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows // 2):
            cursor[ds.key(i)] = value_b
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(30))

        # Now try reading the checkpoint.
        msg = '/Invalid argument/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.operate(ckpt_cursor), msg)

if __name__ == '__main__':
    wttest.run()
