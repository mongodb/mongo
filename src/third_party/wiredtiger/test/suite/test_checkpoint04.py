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
from wtscenario import make_scenarios

class test_checkpoint04(wttest.WiredTigerTestCase):
    ckpt_precision = [
        ('fuzzy', dict(ckpt_config='precise_checkpoint=false')),
        ('precise', dict(ckpt_config='precise_checkpoint=true')),
    ]

    scenarios = make_scenarios(ckpt_precision)

    def conn_config(self):
        return 'cache_size=50MB,statistics=(all),' + self.ckpt_config

    def create_tables(self, ntables):
        tables = {}
        for i in range(0, ntables):
            uri = 'table:table' + str(i)
            ds = SimpleDataSet(self, uri, 0, key_format="i", value_format="S")
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
        ntables = 50
        multiplier = 1

        # Avoid checkpoint error with precise checkpoint
        if self.ckpt_config == 'precise_checkpoint=true':
            self.conn.set_timestamp('stable_timestamp=1')

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
            num_ckpt = self.get_stat(stat.conn.checkpoints_api)
            self.pr('checkpoint, number of checkpoints started by api ' + str(num_ckpt))
            running = self.get_stat(stat.conn.checkpoint_state)
            self.pr('checkpoint_state ' + str(running))
            prep_running = self.get_stat(stat.conn.checkpoint_prep_running)
            self.pr('checkpoint_prep_running ' + str(prep_running))

            prep_min = self.get_stat(stat.conn.checkpoint_prep_min)
            self.pr('checkpoint_prep_min ' + str(prep_min))
            time_min = self.get_stat(stat.conn.checkpoint_time_min)
            self.pr('checkpoint_time_min ' + str(time_min))

            prep_max = self.get_stat(stat.conn.checkpoint_prep_max)
            self.pr('checkpoint_prep_max ' + str(prep_max))
            time_max = self.get_stat(stat.conn.checkpoint_time_max)
            self.pr('checkpoint_time_max ' + str(time_max))

            prep_recent = self.get_stat(stat.conn.checkpoint_prep_recent)
            self.pr('checkpoint_prep_recent ' + str(prep_recent))
            time_recent = self.get_stat(stat.conn.checkpoint_time_recent)
            self.pr('checkpoint_time_recent ' + str(time_recent))

            prep_total = self.get_stat(stat.conn.checkpoint_prep_total)
            self.pr('checkpoint_prep_total ' + str(prep_total))
            time_total = self.get_stat(stat.conn.checkpoint_time_total)
            self.pr('checkpoint_time_total ' + str(time_total))

            # Account for When the connection re-opens on an existing datable as we perform a
            # checkpoint during the open stage.
            expected_ckpts = 3 if multiplier > 1 else 2
            self.assertEqual(num_ckpt, expected_ckpts)
            self.assertEqual(running, 0)
            self.assertEqual(prep_running, 0)
            # Assert if this loop continues for more than 100 iterations.
            self.assertLess(multiplier, 100)

            # This condition is mainly to confirm that prep's stats time are always less than time's stats time.
            # Run the loop again if any of the below condition fails and exit if the test passes.
            if prep_min < time_min and prep_max < time_max and prep_recent < time_recent and prep_total < time_total:
                break

            multiplier += 1
            # Reopen the connection to reset statistics.
            # We don't want stats from earlier runs to interfere with later runs.
            self.reopen_conn()
