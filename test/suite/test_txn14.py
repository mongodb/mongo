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
# test_txn14.py
#   Transactions: commits and rollbacks
#

import fnmatch, os, shutil, time
from suite_subprocess import suite_subprocess
from wtscenario import multiply_scenarios, number_scenarios, prune_scenarios
import wttest

class test_txn14(wttest.WiredTigerTestCase, suite_subprocess):
    t1 = 'table:test_txn14_1'
    create_params = 'key_format=i,value_format=i'
    entries = 10000
    extra_entries = 5
    conn_config = 'log=(archive=false,enabled,file_max=100K)'

    sync_list = [
        ('write', dict(sync='off')),
        ('sync', dict(sync='on')),
        ('bg', dict(sync='background')),
    ]
    scenarios = multiply_scenarios('.', sync_list)

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
        #
        # close the original connection and open to new directory
        # NOTE:  This really cannot test the difference between the
        # write-no-sync (off) version of log_flush and the sync
        # version since we're not crashing the system itself.
        #
        self.close_conn()
        self.conn = self.setUpConnectionOpen(newdir)
        self.session = self.setUpSessionOpen(self.conn)

    def test_log_flush(self):
        # Here's the strategy:
        #    - Create a table.
        #    - Insert data into table.
        #    - Call log_flush.
        #    - Simulate a crash and restart
        #    - Make recovery run.
        #    - Confirm flushed data is in the table.
        #
        self.session.create(self.t1, self.create_params)
        c = self.session.open_cursor(self.t1, None, None)
        for i in range(self.entries):
            c[i] = i + 1
        cfgarg='sync=%s' % self.sync
        self.pr('cfgarg ' + cfgarg)
        self.session.log_flush(cfgarg)
        for i in range(self.extra_entries):
            c[i+self.entries] = i + self.entries + 1
        c.close()
        self.session.log_flush(cfgarg)
        if self.sync == 'background':
            # If doing a background flush, wait a few seconds.  I have
            # seen an individual log file's fsync take more than a second
            # on some systems.  So give it time to flush perhaps a few files.
            self.session.transaction_sync('timeout_ms=4000')
        self.simulate_crash_restart(".", "RESTART")
        c = self.session.open_cursor(self.t1, None, None)
        i = 0
        for key, value in c:
            self.assertEqual(i, key)
            self.assertEqual(i+1, value)
            i += 1
        all = self.entries + self.extra_entries
        self.assertEqual(i, all)
        c.close()

if __name__ == '__main__':
    wttest.run()
