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
# test_backup08.py
#   Timestamps: Verify the saved checkpoint timestamp survives a backup.
#

import os, shutil
import wiredtiger, wttest
from wtscenario import make_scenarios

class test_backup08(wttest.WiredTigerTestCase):
    conn_config = 'config_base=false,create,log=(enabled)'
    dir = 'backup.dir'
    coll1_uri = 'table:collection10.1'
    coll2_uri = 'table:collection10.2'
    coll3_uri = 'table:collection10.3'
    oplog_uri = 'table:oplog10'

    nentries = 10
    table_cnt = 3

    types = [
        ('all', dict(use_stable='false')),
        ('default', dict(use_stable='default')),
        ('stable', dict(use_stable='true')),
    ]
    scenarios = make_scenarios(types)

    def data_and_checkpoint(self):
        #
        # Create several collection-like tables that are checkpoint durability.
        # Add data to each of them separately and checkpoint so that each one
        # has a different stable timestamp.
        #
        self.session.create(self.oplog_uri, 'key_format=i,value_format=i')
        self.session.create(self.coll1_uri, 'key_format=i,value_format=i,log=(enabled=false)')
        self.session.create(self.coll2_uri, 'key_format=i,value_format=i,log=(enabled=false)')
        self.session.create(self.coll3_uri, 'key_format=i,value_format=i,log=(enabled=false)')
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
            # Default is to use the stable timestamp.
            expected = ts
            ckpt_config = ''
            if self.use_stable == 'false':
                expected = 0
                ckpt_config = 'use_timestamp=false'
            elif self.use_stable == 'true':
                ckpt_config = 'use_timestamp=true'
            self.session.checkpoint(ckpt_config)
            q = self.conn.query_timestamp('get=last_checkpoint')
            self.assertTimestampsEqual(q, self.timestamp_str(expected))
        return expected

    def backup_and_recover(self, expected_rec_ts):
        #
        # Perform a live backup. Then open the backup and query the
        # recovery timestamp verifying it is the expected value.
        #
        os.mkdir(self.dir)
        cursor = self.session.open_cursor('backup:')
        while True:
            ret = cursor.next()
            if ret != 0:
                break
            shutil.copy(cursor.get_key(), self.dir)
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        cursor.close()

        backup_conn = self.wiredtiger_open(self.dir)
        q = backup_conn.query_timestamp('get=recovery')
        self.pr("query recovery ts: " + q)
        self.assertTimestampsEqual(q, self.timestamp_str(expected_rec_ts))

    def test_timestamp_backup(self):
        # Add some data and checkpoint using the timestamp or not
        # depending on the configuration. Get the expected timestamp
        # where the data is checkpointed for the backup.
        ckpt_ts = self.data_and_checkpoint()

        # Backup and run recovery checking the recovery timestamp.
        # This tests that the stable timestamp information is transferred
        # with the backup. It should be part of the backup metadata file.
        self.backup_and_recover(ckpt_ts)

if __name__ == '__main__':
    wttest.run()
