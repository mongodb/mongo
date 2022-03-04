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
# test_checkpoint04.py
# Test that the checkpoints timing statistics are populated as expected.

import wttest
from wiredtiger import stat
from wtdataset import SimpleDataSet

class test_checkpoint04(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB,log=(enabled),statistics=(all)'

    def create_tables(self, ntables):
        tables = {}
        for i in range(0, ntables):
            uri = 'table:table' + str(i)
            ds = SimpleDataSet(
                self, uri, 0, key_format="i", value_format="S", config='log=(enabled=false)')
            ds.populate()
            tables[uri] = ds
        return tables

    def add_updates(self, uri, ds, value, nrows):
        session = self.session
        cursor = session.open_cursor(uri)
        self.pr('update: ' + uri + ' for ' + str(nrows) + ' rows')
        for i in range(0, nrows):
            session.begin_transaction()
            cursor[ds.key(i)] = value
            session.commit_transaction()
        cursor.close()

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def test_checkpoint_stats(self):
        nrows = 100
        ntables = 10
        multiplier = 1

        # Run the loop and increase the value size with each iteration until
        # the test passes.
        while True:
            # Create many tables and perform many updates so our checkpoint stats are populated.
            value = "wired" * 100 * multiplier
            tables = self.create_tables(ntables)
            for uri, ds in tables.items():
                self.add_updates(uri, ds, value, nrows)

            # Perform a checkpoint.
            self.session.checkpoint()

            # Update the tables.
            value = "tiger" * 100 * multiplier
            tables = self.create_tables(ntables)
            for uri, ds in tables.items():
                self.add_updates(uri, ds, value, nrows)

            # Perform a checkpoint.
            self.session.checkpoint()

            # Check the statistics.
            # Set them into a variable so that we can print them all out. We've had a failure
            # on Windows that is very difficult to reproduce so collect what info we can.
            num_ckpt = self.get_stat(stat.conn.txn_checkpoint)
            self.pr('txn_checkpoint, number of checkpoints ' + str(num_ckpt))
            running = self.get_stat(stat.conn.txn_checkpoint_running)
            self.pr('txn_checkpoint_running ' + str(running))
            prep_running = self.get_stat(stat.conn.txn_checkpoint_prep_running)
            self.pr('txn_checkpoint_prep_running ' + str(prep_running))

            prep_min = self.get_stat(stat.conn.txn_checkpoint_prep_min)
            self.pr('txn_checkpoint_prep_min ' + str(prep_min))
            time_min = self.get_stat(stat.conn.txn_checkpoint_time_min)
            self.pr('txn_checkpoint_time_min ' + str(time_min))

            prep_max = self.get_stat(stat.conn.txn_checkpoint_prep_max)
            self.pr('txn_checkpoint_prep_max ' + str(prep_max))
            time_max = self.get_stat(stat.conn.txn_checkpoint_time_max)
            self.pr('txn_checkpoint_time_max ' + str(time_max))

            prep_recent = self.get_stat(stat.conn.txn_checkpoint_prep_recent)
            self.pr('txn_checkpoint_prep_recent ' + str(prep_recent))
            time_recent = self.get_stat(stat.conn.txn_checkpoint_time_recent)
            self.pr('txn_checkpoint_time_recent ' + str(time_recent))

            prep_total = self.get_stat(stat.conn.txn_checkpoint_prep_total)
            self.pr('txn_checkpoint_prep_total ' + str(prep_total))
            time_total = self.get_stat(stat.conn.txn_checkpoint_time_total)
            self.pr('txn_checkpoint_time_total ' + str(time_total))

            self.assertEqual(num_ckpt, 2 * multiplier)
            self.assertEqual(running, 0)
            self.assertEqual(prep_running, 0)
            # Assert if this loop continues for more than 100 iterations.
            self.assertLess(multiplier, 100)

            # This condition is mainly to confirm that prep's stats time are always less than time's stats time.
            # Run the loop again if any of the below condition fails and exit if the test passes.
            if prep_min < time_min and prep_max < time_max and prep_recent < time_recent and prep_total < time_total:
                break
            else:
                multiplier += 1

if __name__ == '__main__':
    wttest.run()
