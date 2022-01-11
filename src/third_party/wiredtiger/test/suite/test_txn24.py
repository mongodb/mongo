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
# test_txn24.py
#   Transactions and eviction: Test if using snapshot isolation for eviction threads helps with
#   cache stuck issue.
#

import wiredtiger, wttest
import time
from wtscenario import make_scenarios

class test_txn24(wttest.WiredTigerTestCase):


    table_params_values = [
        ('integer-row', dict(key_format='i', value_format='S', extraconfig='')),
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('column-fix', dict(key_format='r', value_format='8t', extraconfig=',leaf_page_max=4096')),
    ]
    scenarios = make_scenarios(table_params_values)

    def conn_config(self):
        # We want to either eliminate or keep the application thread role in eviction to minimum.
        # This will ensure that the dedicated eviction threads are doing the heavy lifting.
        return 'cache_size=100MB,eviction_target=80,eviction_dirty_target=5,eviction_trigger=100,\
                eviction_updates_target=5,eviction_dirty_trigger=99,eviction_updates_trigger=100,\
                eviction=(threads_max=4)'

    def test_snapshot_isolation_and_eviction(self):

        # Create and populate a table.
        uri = "table:test_txn24"
        table_params = 'key_format={},value_format={}'.format(self.key_format, self.value_format)

        if self.value_format == '8t':
            # Values are 1/240 the size, but as in-memory updates are considerably larger, we
            # shouldn't just use 240x the number of rows. For now go with 3x, a number pulled from
            # thin air that also makes it just about 3x slower than the VLCS case. It isn't clear
            # whether it's really working as intended, and should maybe check deeper or use some
            # kind of stats feedback to figure out how many rows to pump out instead of choosing
            # in advance.
            default_val = 45
            new_val = 101
            n_rows = 480000 * 3
        else:
            default_val = 'ABCD' * 60
            new_val = 'YYYY' * 60
            n_rows = 480000

        self.session.create(uri, table_params + self.extraconfig)
        cursor = self.session.open_cursor(uri, None)
        for i in range(1, n_rows + 1):
            cursor[i] = default_val
        cursor.close()

        # Perform a checkpoint. There should be no dirty content in the cache after this.
        self.session.checkpoint()

        # Start a transaction, make an update and keep it running.
        cursor = self.session.open_cursor(uri, None)
        self.session.begin_transaction()
        cursor[1] = new_val

        # Start few sessions and transactions, make updates and try committing them.
        session2 = self.setUpSessionOpen(self.conn)
        cursor2 = session2.open_cursor(uri)
        start_row = n_rows // 4
        for i in range(0, n_rows // 4):
            cursor2[start_row] = new_val
            start_row += 1

        session3 = self.setUpSessionOpen(self.conn)
        cursor3 = session3.open_cursor(uri)
        start_row = n_rows // 2
        for i in range(0, n_rows // 4):
            cursor3[start_row] = new_val
            start_row += 1

        # At this point in time, we have made roughly 90% cache dirty. If we are not using
        # snapshots for eviction threads, the cache state will remain like this forever and we may
        # never reach this part of code. We might get a rollback error by now or WT will panic with
        # cache stuck error.
        #
        # Even if we don't get an error by now and if we try to insert new data at this point in
        # time, dirty cache usage will exceed 100% if eviction threads are not using snapshot
        # isolation. In that case, we will eventually get a rollback error for sure.

        session4 = self.setUpSessionOpen(self.conn)
        cursor4 = session4.open_cursor(uri)
        start_row = 2
        for i in range(0, n_rows // 4):
            cursor4[start_row] = new_val
            start_row += 1

        # If we have done all operations error free so far, eviction threads have been successful.

        self.session.commit_transaction()
        cursor.close()
        self.session.close()

        cursor2.close()
        session2.close()

        cursor3.close()
        session3.close()

        cursor4.close()
        session4.close()

if __name__ == '__main__':
    wttest.run()
