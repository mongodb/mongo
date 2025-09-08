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
# test_prepare_discover02.py
#   Test that pending prepared transaction artifacts can be discovered after recovery
#   and committed

import wiredtiger
from suite_subprocess import suite_subprocess
import wttest
from wtscenario import make_scenarios

class test_prepare_discover02(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_prepare_discover02'
    uri = 'table:' + tablename
    conn_config = 'precise_checkpoint=true,preserve_prepared=true'

    types = [
        ('row', dict(s_config='key_format=i,value_format=S')),
    ]

    # Transaction end types
    txn_end = [
        ('txn_commit', dict(txn_commit=True)),
    ]

    scenarios = make_scenarios(types, txn_end)

    @wttest.only_for_hook("disagg", "FIXME-WT-15343 disable RTS when precise checkpoint is on")
    def test_prepare_discover02(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(50))
        self.session.create(self.uri, self.s_config)
        c = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        c[1] = "commit ts=60"
        c[2] = "commit ts=60"
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(60))

        # Insert a few keys then prepare the transaction
        self.session.begin_transaction()
        c[3] = "prepare ts=100"
        c[4] = "prepare ts=100"
        c[5] = "prepare ts=100"
        # Prepare with a timestamp greater than current stable
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(100) +',prepared_id=' + self.prepared_id_str(123))
        # Move the stable timestamp to include the prepared transaction
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(150))
        # Create a checkpoint
        session2 = self.conn.open_session()
        session2.checkpoint()

        # Creating backup that will preserve artifacts
        backup_dir = 'bkp'
        self.backup(backup_dir, session2)

        # Opening backup database
        conn2 = self.wiredtiger_open(backup_dir, self.conn_config)

        c2s1 = conn2.open_session()

        # Opening prepared discover cursor
        prepared_discover_cursor = c2s1.open_cursor("prepared_discover:")

        # Walking through prepared discover cursor
        c2s2 = conn2.open_session()
        count = 0
        while prepared_discover_cursor.next() == 0:
            count += 1
            prepared_id = prepared_discover_cursor.get_key()
            self.assertEqual(prepared_id, 123)
            c2s2.begin_transaction("claim_prepared=" + self.timestamp_str(prepared_id))
            c2s2.commit_transaction("commit_timestamp=" + self.timestamp_str(200)+",durable_timestamp=" + self.timestamp_str(210))
        self.assertEqual(count, 1)
        prepared_discover_cursor.close()

        # Read cursor
        c2s3 = conn2.open_session()

        # check the read cursor can only find values committed at timestamp = 60
        c2s3.begin_transaction(f'read_timestamp={self.timestamp_str(60)}')
        read_cursor = c2s3.open_cursor(self.uri, None, None)
        for i in range(1, 3):
            read_cursor.set_key(i)
            self.assertEqual(read_cursor.search(), 0)
            self.assertEqual(read_cursor.get_value(), "commit ts=60")
        for i in range(3, 6):
            read_cursor.set_key(i)
            self.assertEqual(read_cursor.search(), wiredtiger.WT_NOTFOUND)
        c2s3.rollback_transaction()

        # check the read cursor can read all keys after timestamp 200
        c2s3.begin_transaction(f'read_timestamp={self.timestamp_str(200)}')
        read_cursor = c2s3.open_cursor(self.uri, None, None)
        for i in range(1, 3):
            read_cursor.set_key(1)
            self.assertEqual(read_cursor.search(), 0)
            self.assertEqual(read_cursor.get_value(), "commit ts=60")

        for i in range(3, 6):
            read_cursor.set_key(3)
            self.assertEqual(read_cursor.search(), 0)
            self.assertEqual(read_cursor.get_value(), "prepare ts=100")
        c2s3.rollback_transaction()
