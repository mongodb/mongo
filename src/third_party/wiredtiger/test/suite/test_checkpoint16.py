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

# test_checkpoint16.py
#
# Make sure a table that's clean when a checkpointed can still be read in
# that checkpoint.

class test_checkpoint(wttest.WiredTigerTestCase):
    session_config = 'isolation=snapshot'

    format_values = [
        ('column-fix', dict(key_format='r', value_format='8t',
            extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('string_row', dict(key_format='S', value_format='S', extraconfig='')),
    ]
    name_values = [
        ('named', dict(second_checkpoint='second_checkpoint')),
        ('unnamed', dict(second_checkpoint=None)),
    ]
    scenarios = make_scenarios(format_values, name_values)

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

    def check(self, ds, ckpt, nrows, value):
        if ckpt is None:
            ckpt = 'WiredTigerCheckpoint'
        cursor = self.session.open_cursor(ds.uri, None, 'checkpoint=' + ckpt)
        #self.session.begin_transaction()
        count = 0
        for k, v in cursor:
            self.assertEqual(v, value)
            count += 1
        self.assertEqual(count, nrows)
        #self.session.rollback_transaction()
        cursor.close()

    def test_checkpoint(self):
        uri1 = 'table:checkpoint16a'
        uri2 = 'table:checkpoint16b'
        nrows = 1000

        # Create two tables.
        ds1 = SimpleDataSet(
            self, uri1, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds1.populate()
        ds2 = SimpleDataSet(
            self, uri2, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds2.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100

        # Set oldest and stable to 5.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(5) +
            ',stable_timestamp=' + self.timestamp_str(5))

        # Write some data to both tables and checkpoint it.
        self.large_updates(ds1, nrows, value_a)
        self.large_updates(ds2, nrows, value_a)
        self.session.checkpoint()

        # Write some more data but only to table 2.
        self.large_updates(ds2, nrows, value_b)

        # Checkpoint this data.
        self.do_checkpoint(self.second_checkpoint)

        # Make sure we can read table 1 from the second checkpoint.
        self.check(ds1, self.second_checkpoint, nrows, value_a)

if __name__ == '__main__':
    wttest.run()
