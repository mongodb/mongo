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
# test_txn16.py
#   Recovery: Test that toggling between logging and not logging does not
#   continue to generate more log files.
#

import fnmatch, os, shutil, time
from suite_subprocess import suite_subprocess
import wttest

class test_txn16(wttest.WiredTigerTestCase, suite_subprocess):
    t1 = 'table:test_txn16_1'
    t2 = 'table:test_txn16_2'
    t3 = 'table:test_txn16_3'
    nentries = 1000
    create_params = 'key_format=i,value_format=i'
    # Set the log file size small so we generate checkpoints
    # with LSNs in different files.
    conn_config = 'config_base=false,' + \
        'log=(enabled,file_max=100K,remove=false),' + \
        'transaction_sync=(method=dsync,enabled)'
    conn_on = 'config_base=false,' + \
        'log=(enabled,file_max=100K,remove=false),' + \
        'transaction_sync=(method=dsync,enabled)'
    conn_off = 'config_base=false,log=(enabled=false)'

    def populate_table(self, uri):
        self.session.create(uri, self.create_params)
        c = self.session.open_cursor(uri, None, None)
        # Populate with an occasional checkpoint to generate
        # some varying LSNs.
        for i in range(self.nentries):
            c[i] = i + 1
            if i % 900 == 0:
                self.session.checkpoint()
        c.close()

    def copy_dir(self, olddir, newdir):
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
        # close the original connection.
        self.close_conn()

    def run_toggle(self, homedir):
        loop = 0
        # Record original log files.  There should never be overlap
        # with these even after they're removed.
        orig_logs = fnmatch.filter(os.listdir(homedir), "*gerLog*")
        while loop < 3:
            # Reopen with logging on to run recovery first time
            on_conn = self.wiredtiger_open(homedir, self.conn_on)
            on_conn.close()
            if loop > 0:
                # Get current log files.
                cur_logs = fnmatch.filter(os.listdir(homedir), "*gerLog*")
                scur = set(cur_logs)
                sorig = set(orig_logs)
                # There should never be overlap with the log files that
                # were there originally.  Mostly this checks that after
                # opening with logging disabled and then re-enabled, we
                # don't see log file 1.
                self.assertEqual(scur.isdisjoint(sorig), True)
                if loop > 1:
                    # We should be creating the same log files each time.
                    for l in cur_logs:
                        self.assertEqual(l in last_logs, True)
                    for l in last_logs:
                        self.assertEqual(l in cur_logs, True)
                last_logs = cur_logs
            loop += 1
            # Remove all log files before opening without logging.
            cur_logs = fnmatch.filter(os.listdir(homedir), "*gerLog*")
            for l in cur_logs:
                path=homedir + "/" + l
                os.remove(path)
            off_conn = self.wiredtiger_open(homedir, self.conn_off)
            off_conn.close()

    def test_recovery(self):
        ''' Check log file creation when toggling. '''

        # Here's the strategy:
        #    - With logging populate 4 tables.  Checkpoint
        #      them at different times.
        #    - Copy to a new directory to simulate a crash.
        #    - Close the original connection.
        #    On both a "copy" to simulate a crash and the original (3x):
        #    - Record log files existing.
        #    - Reopen with logging to run recovery.  Close connection.
        #    - Record log files existing.
        #    - Remove all log files.
        #    - Open connection with logging disabled.
        #    - Record log files existing.  Verify we don't keep adding.
        #
        self.populate_table(self.t1)
        self.populate_table(self.t2)
        self.populate_table(self.t3)
        self.copy_dir(".", "RESTART")
        self.run_toggle(".")
        self.run_toggle("RESTART")

if __name__ == '__main__':
    wttest.run()
