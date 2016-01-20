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
# test_txn11.py
#   Transactions: Empty checkpoints and log archiving

import fnmatch, os, time
from suite_subprocess import suite_subprocess
from helper import simple_populate
import wttest

class test_txn11(wttest.WiredTigerTestCase, suite_subprocess):
    archive = 'true'
    conn_config = 'verbose=[transaction]'
    logmax = "100K"
    nrows = 700
    tablename = 'test_txn11'
    source_uri = 'table:' + tablename + "_src"
    uri = 'table:' + tablename

    # Turn on logging for this test.
    def conn_config(self, dir):
        return 'log=(archive=%s,' % self.archive + \
            'enabled,file_max=%s,prealloc=false),' % self.logmax + \
            'transaction_sync=(enabled=false),'

    def run_checkpoints(self):
        orig_logs = fnmatch.filter(os.listdir(self.home), "*Log*")
        checkpoints = 0
        sorig = set(orig_logs)
        while checkpoints < 500:
            self.session.checkpoint()
            cur_logs = fnmatch.filter(os.listdir(self.home), "*Log*")
            scur = set(cur_logs)
            if scur.isdisjoint(sorig):
                break
            checkpoints += 1
        return


    def test_ops(self):
        # Populate a table
        simple_populate(self, self.source_uri, 'key_format=S', self.nrows)

        # Run forced checkpoints
        self.run_checkpoints()

        self.archive = 'false'
        # Close and reopen the connection
        self.reopen_conn()

if __name__ == '__main__':
    wttest.run()
