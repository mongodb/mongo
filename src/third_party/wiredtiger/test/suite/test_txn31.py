#!/usr/bin/env python3
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

import time
import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_txn31.py
# Snapshot isolation must give repeatable reads across a table close and reopen.
#
# A transaction reading at snapshot isolation without a read timestamp is isolated
# purely by its snapshot: a key committed by another transaction after the snapshot
# was taken must never become visible to it. This must continue to hold even if the
# table's underlying handle is closed and reopened while the transaction is still
# open, e.g. after the reader closes its cursor and the idle handle is swept.
@disagg_test_class
class test_txn31(wttest.WiredTigerTestCase):
    # Let idle handles be closed quickly so the reopen path is exercised.
    conn_base_config = 'create,statistics=(all),' + \
        'file_manager=(close_idle_time=1,close_scan_interval=1,close_handle_minimum=0),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    uri = 'layered:txn31'

    # Number of idle table handles the connection has closed.
    def handles_closed(self):
        stat = self.session.open_cursor('statistics:')
        count = stat[wiredtiger.stat.conn.dh_sweep_expired_close][2]
        stat.close()
        return count

    # Wait until at least one more idle handle has been closed than the given baseline,
    # confirming the table was actually closed so the reopen path is exercised.
    def wait_for_handle_close(self, baseline):
        for _ in range(60):
            time.sleep(1)
            if self.handles_closed() > baseline:
                return
        self.fail('the table handle was not closed, so the reopen path was not exercised')

    def test_repeatable_read_across_handle_reopen(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')

        # An existing durable key, so the table has an on-disk checkpoint.
        c = self.session.open_cursor(self.uri)
        c['key1'] = 'A'
        c.close()
        self.session.checkpoint()

        # Start a snapshot-isolation reader with no read timestamp and take its snapshot
        # by reading. 'key2' does not exist yet, so the reader must not see it.
        reader = self.conn.open_session()
        reader.begin_transaction('isolation=snapshot')
        rc = reader.open_cursor(self.uri)
        rc.set_key('key2')
        self.assertEqual(rc.search(), wiredtiger.WT_NOTFOUND)
        # Release the cursor so the handle can be closed, but keep the transaction open.
        rc.close()

        # Another transaction commits 'key2' after the reader's snapshot was taken, so it
        # is not part of that snapshot and must stay invisible to the reader.
        writer = self.conn.open_session()
        writer.begin_transaction()
        wc = writer.open_cursor(self.uri)
        wc['key2'] = 'B'
        wc.close()
        writer.commit_transaction()

        # Make 'key2' durable and wait for the now-idle table handle to be closed.
        self.session.checkpoint()
        baseline = self.handles_closed()
        self.wait_for_handle_close(baseline)

        # Read again in the still-open reader transaction, which reopens the table. The
        # reader's snapshot still excludes the writer, so 'key2' must remain invisible.
        rc = reader.open_cursor(self.uri)
        rc.set_key('key2')
        self.assertEqual(rc.search(), wiredtiger.WT_NOTFOUND,
            'snapshot reader saw a key committed after its snapshot was taken')
        reader.rollback_transaction()
