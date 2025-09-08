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
# test_prepare_discover03.py
#   Test that prepare discover cursor should return an error when closed with unclaimed prepared transactions

import wiredtiger
from suite_subprocess import suite_subprocess
import wttest
from wtscenario import make_scenarios

class test_prepare_discover03(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_prepare_discover03'
    uri = 'table:' + tablename
    conn_config = 'precise_checkpoint=true,preserve_prepared=true'
    s_config = 'key_format=i,value_format=S'

    @wttest.only_for_hook("disagg", "FIXME-WT-15343 disable RTS when precise checkpoint is on")
    def test_prepare_discover03(self):
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

        session2 = self.conn.open_session()
        c2 = session2.open_cursor(self.uri)
        session2.begin_transaction()
        c2[1] = "prepare ts=100"
        c2[2] = "prepare ts=100"
        session2.prepare_transaction('prepare_timestamp=' + self.timestamp_str(100) +',prepared_id=' + self.prepared_id_str(150))
        # Move the stable timestamp to include the prepared transaction
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(150))
        # Create a checkpoint
        session3 = self.conn.open_session()
        session3.checkpoint()

        # Creating backup that will preserve artifacts
        backup_dir = 'bkp'
        self.backup(backup_dir, session3)

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
            if count == 1:
                break
        self.assertEqual(count, 1)
        # Try to claim an already claimed prepared transaction, should return an error
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: c2s2.begin_transaction("claim_prepared=" + self.timestamp_str(123)), "")
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: prepared_discover_cursor.close(), "/Found 1 unclaimed prepared transactions/")
