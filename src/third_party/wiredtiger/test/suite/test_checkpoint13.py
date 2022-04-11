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

# test_checkpoint13.py: API restrictions on checkpoint cursors
#
# - You may not read from a checkpoint cursor while in a transaction.
# (The checkpoint cursor is its own private transaction.)
#
# - You may not read from a checkpoint prior to its oldest timestamp.
#
# - You may not regen or drop a named checkpoint with a cursor open.

class test_checkpoint(wttest.WiredTigerTestCase):
    conn_config = ''
    session_config = 'isolation=snapshot'

    ckptname_values = [
        ('named', dict(checkpoint_name='my_ckpt')),
        ('unnamed', dict(checkpoint_name=None)),
    ]
    scenarios = make_scenarios(ckptname_values)

    # No need to run this on more than one btree type.
    key_format = 'r'
    value_format = 'S'

    def updates(self, uri, ds, nrows, value, ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[ds.key(i)] = value
            if i % 101 == 0:
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
                self.session.begin_transaction()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()

    def test_checkpoint(self):
        uri = 'table:checkpoint13'
        nrows = 10

        # Create a table.
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
        else:
            value_a = "aaaaa" * 10
            value_b = "bbbbb" * 10
            value_c = "ccccc" * 10

        # Set oldest and stable timestamps to 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        # Write some data at time 20.
        self.updates(uri, ds, nrows, value_a, 20)

        # Make it stable.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        
        # Make a checkpoint.
        if self.checkpoint_name is not None:
            self.session.checkpoint('name=' + self.checkpoint_name)
            read_name = self.checkpoint_name
        else:
            self.session.checkpoint()
            read_name = 'WiredTigerCheckpoint'

        # Write some more data at time 30 to make sure it's not seen.
        self.updates(uri, ds, nrows, value_b, 30)

        # Open the checkpoint.
        ckpt_cursor = self.session.open_cursor(uri, None, 'checkpoint=' + read_name)

        # We should be able to read.
        self.assertEqual(ckpt_cursor[ds.key(1)], value_a)

        # We should also able to read within a transaction.
        self.session.begin_transaction()
        self.assertEqual(ckpt_cursor[ds.key(1)], value_a)
        self.session.rollback_transaction()

        # Close this cursor.
        ckpt_cursor.close()

        # Opening the cursor at time 10 should produce no data.
        ckpt_cursor = self.session.open_cursor(uri, None, 'checkpoint=' + read_name +
                ',debug=(checkpoint_read_timestamp=' + self.timestamp_str(10) + ')')
        ckpt_cursor.set_key(ds.key(1))
        self.assertEqual(ckpt_cursor.search(), wiredtiger.WT_NOTFOUND)
        ckpt_cursor.close()

        # Opening the cursor at time 5 should fail.
        def tryit():
            return self.session.open_cursor(uri, None, 'checkpoint=' + read_name +
                    ',debug=(checkpoint_read_timestamp=' + self.timestamp_str(5) + ')')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: tryit(), '/before the checkpoint oldest/')

        if self.checkpoint_name is not None:
            # Open the cursor.
            ckpt_cursor = self.session.open_cursor(uri, None, 'checkpoint=' + read_name)

            # Updating the checkpoint should fail.
            def tryregen():
                self.session.checkpoint('name=' + self.checkpoint_name)
            # This produces EBUSY, but self.raisesBusy() from wttest does not work.
            # Including "dropped" in the expected message is not optimal, since we are't
            # dropping the checkpoint (that regenerating it drops it first is an internal
            # detail) but I guess it can't be helped.
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: tryregen(), '/cannot be dropped/')

            # Dropping the checkpoint should fail.
            def trydrop():
                self.session.checkpoint('drop=(' + self.checkpoint_name + ')')
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: trydrop(), '/cannot be dropped/')

            ckpt_cursor.close()
            

if __name__ == '__main__':
    wttest.run()
