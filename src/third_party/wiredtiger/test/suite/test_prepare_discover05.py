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
# test_prepare_discover04.py
#   Test that prepare discover cursor can discover and commit a pending prepared delete

import wiredtiger
from suite_subprocess import suite_subprocess
import wttest
from wtscenario import make_scenarios

class test_prepare_discover04(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_prepare_discover04'
    uri = 'table:' + tablename
    conn_config = 'precise_checkpoint=true,preserve_prepared=true'
    s_config = 'key_format=i,value_format=S'

    txn_end = [
        ('txn_commit', dict(txn_commit=True)),
        ('txn_rollback', dict(txn_commit=False)),
    ]
    scenarios = make_scenarios(txn_end)

    def test_prepare_discover04(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(50))
        self.session.create(self.uri, self.s_config)
        c = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        c[1] = "commit ts=60"
        c[2] = "commit ts=60"
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(60))

        session2 = self.conn.open_session()
        c2 = session2.open_cursor(self.uri)
        session2.begin_transaction()
        c2.set_key(1)
        self.assertEqual(c2.remove(), 0)
        c2.set_key(2)
        self.assertEqual(c2.remove(), 0)
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
            c2s2.begin_transaction("claim_prepared_id=" + self.timestamp_str(prepared_id))
            if self.txn_commit == True:
                c2s2.commit_transaction("commit_timestamp=" + self.timestamp_str(200)+",durable_timestamp=" + self.timestamp_str(210))
            else:
                c2s2.rollback_transaction("rollback_timestamp=" + self.timestamp_str(200))

        self.assertEqual(count, 1)

        # Force eviction to trigger reconciliation, since we haven't moved stable timestamp, all updates should be saved to disk
        # as prepared
        session_evict = conn2.open_session("debug=(release_evict_page=true)")
        session_evict.begin_transaction("ignore_prepare=true")
        evict_cursor = session_evict.open_cursor(self.uri, None, None)
        for i in range(1, 2):
            evict_cursor.set_key(i)
            evict_cursor.search()
            evict_cursor.reset()
        evict_cursor.close()
        session_evict.rollback_transaction()
        session_evict.close()

        # Calling checkpoint, as part of disk verification it will try to unpack cells that were written by eviction.
        # Unpacking these cells should not cause the program to crash
        c2s2.checkpoint()
