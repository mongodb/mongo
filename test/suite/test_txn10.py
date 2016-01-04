#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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
# test_txn10.py
#   Transactions: commits and rollbacks
#

import fnmatch, os, shutil, time
from suite_subprocess import suite_subprocess
from wtscenario import multiply_scenarios, number_scenarios, prune_scenarios
import wttest

class test_txn10(wttest.WiredTigerTestCase, suite_subprocess):
    t1 = 'table:test_txn10_1'
    t2 = 'table:test_txn10_2'
    create_params = 'key_format=i,value_format=i'
    conn_config = 'log=(archive=false,enabled,file_max=100K),' + \
                'transaction_sync=(method=dsync,enabled)'

    def simulate_crash_restart(self, olddir, newdir):
        ''' Simulate a crash from olddir and restart in newdir. '''
        # with the connection still open, copy files to new directory
        shutil.rmtree(newdir, ignore_errors=True)
        os.mkdir(newdir)
        for fname in os.listdir(olddir):
            fullname = os.path.join(olddir, fname)
            # Skip lock file on Windows since it is locked
            if os.path.isfile(fullname) and \
                "WiredTiger.lock" not in fullname and \
                "Tmplog" not in fullname and \
                "Preplog" not in fullname:
                shutil.copy(fullname, newdir)
        # close the original connection and open to new directory
        self.close_conn()
        self.conn = self.setUpConnectionOpen(newdir)
        self.session = self.setUpSessionOpen(self.conn)

    def test_recovery(self):
        ''' Check for bugs in file ID allocation. '''

        # Here's the strategy:
        #    - Create a table (t1).
        #    - Do a clean restart.
        #    - Create another table (t2).
        #    - Insert data into t2.
        #    - Make recovery run.
        #
        # If we aren't tracking file IDs properly, it's possible that
        # we'd end up apply the log records for t2 to table t1.
        self.session.create(self.t1, self.create_params)
        self.reopen_conn()
        self.session.create(self.t2, self.create_params)
        c = self.session.open_cursor(self.t2, None, None)
        for i in range(10000):
            c[i] = i + 1
        c.close()
        self.simulate_crash_restart(".", "RESTART")
        c = self.session.open_cursor(self.t2, None, None)
        i = 0
        for key, value in c:
            self.assertEqual(i, key)
            self.assertEqual(i+1, value)
            i += 1
        self.assertEqual(i, 10000)
        c.close()
        c = self.session.open_cursor(self.t1, None, None)
        i = 0
        for key, value in c:
            i += 1
        self.assertEqual(i, 0)
        c.close()

if __name__ == '__main__':
    wttest.run()
