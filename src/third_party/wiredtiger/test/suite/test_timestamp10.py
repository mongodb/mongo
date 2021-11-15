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
# test_timestamp10.py
#   Timestamps: Saving and querying the last checkpoint and recovery timestamps.
#

from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wtscenario import make_scenarios

class test_timestamp10(wttest.WiredTigerTestCase, suite_subprocess):
    conn_config = 'config_base=false,create,log=(enabled)'
    session_config = 'isolation=snapshot'
    coll1_uri = 'table:collection10.1'
    coll2_uri = 'table:collection10.2'
    coll3_uri = 'table:collection10.3'
    oplog_uri = 'table:oplog10'

    nentries = 10
    table_cnt = 3

    format_values = [
        ('integer-row', dict(key_format='i', value_format='i')),
        ('column', dict(key_format='r', value_format='i')),
        ('column-fix', dict(key_format='r', value_format='8t')),
    ]
    types = [
        ('all', dict(use_stable='false', run_wt=0)),
        ('all+wt', dict(use_stable='false', run_wt=1)),
        ('all+wt2', dict(use_stable='false', run_wt=2)),
        ('default', dict(use_stable='default', run_wt=0)),
        ('default+wt', dict(use_stable='default', run_wt=1)),
        ('default+wt2', dict(use_stable='default', run_wt=2)),
        ('stable', dict(use_stable='true', run_wt=0)),
        ('stable+wt', dict(use_stable='true', run_wt=1)),
        ('stable+wt2', dict(use_stable='true', run_wt=2)),
    ]
    scenarios = make_scenarios(format_values, types)

    def data_and_checkpoint(self):
        #
        # Create several collection-like tables that are checkpoint durability.
        # Add data to each of them separately and checkpoint so that each one
        # has a different stable timestamp.
        #
        basecfg = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(self.oplog_uri, basecfg)
        self.session.create(self.coll1_uri, basecfg + ',log=(enabled=false)')
        self.session.create(self.coll2_uri, basecfg + ',log=(enabled=false)')
        self.session.create(self.coll3_uri, basecfg + ',log=(enabled=false)')
        c_op = self.session.open_cursor(self.oplog_uri)
        c = []
        c.append(self.session.open_cursor(self.coll1_uri))
        c.append(self.session.open_cursor(self.coll2_uri))
        c.append(self.session.open_cursor(self.coll3_uri))

        # Begin by adding some data.
        for table in range(1,self.table_cnt+1):
            curs = c[table - 1]
            start = self.nentries * table
            end = start + self.nentries
            ts = (end - 3)
            self.pr("table: " + str(table))
            for i in range(start,end):
                self.session.begin_transaction()
                c_op[i] = i
                curs[i] = i
                self.pr("i: " + str(i))
                self.session.commit_transaction(
                  'commit_timestamp=' + self.timestamp_str(i))
            # Set the oldest and stable timestamp a bit earlier than the data
            # we inserted. Take a checkpoint to the stable timestamp.
            self.pr("stable ts: " + str(ts))
            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(ts) +
                ',stable_timestamp=' + self.timestamp_str(ts))
            # This forces a different checkpoint timestamp for each table.
            self.session.checkpoint()
            q = self.conn.query_timestamp('get=last_checkpoint')
            self.assertTimestampsEqual(q, self.timestamp_str(ts))
        return ts

    def close_and_recover(self, expected_rec_ts):
        #
        # Close with the close configuration string and optionally run
        # the 'wt' command. Then open the connection again and query the
        # recovery timestamp verifying it is the expected value.
        #
        if self.use_stable == 'true':
            close_cfg = 'use_timestamp=true'
        elif self.use_stable == 'false':
            close_cfg = 'use_timestamp=false'
        else:
            close_cfg = ''
        self.close_conn(close_cfg)

        # Run the wt command some number of times to get some runs in that do
        # not use timestamps. Make sure the recovery checkpoint is maintained.
        for i in range(0, self.run_wt):
            self.runWt(['-C', 'config_base=false,create,log=(enabled)', '-h', '.', '-R', 'list', '-v'], outfilename="list.out")

        self.open_conn()
        q = self.conn.query_timestamp('get=recovery')
        self.pr("query recovery ts: " + q)
        self.assertTimestampsEqual(q, self.timestamp_str(expected_rec_ts))

    def test_timestamp_recovery(self):
        # Add some data and checkpoint at a stable timestamp.
        last_stable = self.data_and_checkpoint()

        expected = 0
        # Note: assumes default is true.
        if self.use_stable != 'false':
            expected = last_stable
        # Close and run recovery checking the stable timestamp.
        self.close_and_recover(expected)

        # Verify the data in the recovered database.
        c_op = self.session.open_cursor(self.oplog_uri)
        c = []
        c.append(self.session.open_cursor(self.coll1_uri))
        c.append(self.session.open_cursor(self.coll2_uri))
        c.append(self.session.open_cursor(self.coll3_uri))
        for table in range(1,self.table_cnt+1):
            curs = c[table - 1]
            start = self.nentries * table
            end = start + self.nentries
            ts = (end - 3)
            for i in range(start,end):
                # The oplog-like table is logged so it always has all the data.
                self.assertEquals(c_op[i], i)
                curs.set_key(i)
                # Earlier tables have all the data because later checkpoints
                # will save the last bit of data. Only the last table will
                # be missing some.
                if self.use_stable == 'false' or i <= ts or table != self.table_cnt:
                    self.assertEquals(curs[i], i)
                elif self.value_format == '8t':
                    # For FLCS, expect the table to have extended under the lost values.
                    # We should see 0 and not the data that was written.
                    self.assertEqual(curs.search(), 0)
                    self.assertEqual(curs.get_value(), 0)
                else:
                    self.assertEqual(curs.search(), wiredtiger.WT_NOTFOUND)

if __name__ == '__main__':
    wttest.run()
