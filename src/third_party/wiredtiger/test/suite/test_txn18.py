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
# test_txn18.py
#   Transactions: test recovery settings
#

from suite_subprocess import suite_subprocess
import helper, wiredtiger, wttest
from wtscenario import make_scenarios

class test_txn18(wttest.WiredTigerTestCase, suite_subprocess):
    t1 = 'table:test_txn18'
    conn_config = 'log=(enabled,file_max=100K,remove=false),' + \
                'transaction_sync=(method=dsync,enabled)'
    conn_recerror = conn_config + ',log=(recover=error)'
    conn_recon = conn_config + ',log=(recover=on)'

    format_values = [
        ('integer-row', dict(key_format='i', value_format='i')),
        ('column', dict(key_format='r', value_format='i')),
        ('column-fix', dict(key_format='r', value_format='8t')),
    ]
    scenarios = make_scenarios(format_values)

    def mkvalue(self, i):
        if self.value_format == '8t':
            return i % 256
        return i

    def test_recovery(self):
        ''' Run the recovery settings '''

        # Here's the strategy:
        #    - Create a table (t1).
        #    - Insert data into t1.
        #    - Simulate a crash.
        #    - Make recovery run with recovery=error
        # and make sure it detects an error since recovery is needed
        #    - Make recovery run with recovery=on.
        #    - Do a clean shutdown and restart with recovery=error
        # and make sure is successful.
        #
        # If we aren't tracking file IDs properly, it's possible that
        # we'd end up apply the log records for t2 to table t1.
        create_params = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(self.t1, create_params)
        #
        # Since we're logging, we need to flush out the meta-data file
        # from the create.
        self.session.checkpoint()
        c = self.session.open_cursor(self.t1, None, None)
        for i in range(1, 10001):
            c[i] = self.mkvalue(i + 1)
        c.close()
        olddir = "."
        newdir = "RESTART"
        errdir = "ERROR"
        helper.copy_wiredtiger_home(self, olddir, errdir)
        helper.copy_wiredtiger_home(self, olddir, newdir)
        # close the original connection
        self.close_conn()
        # Trying to open the error directory with recover=error should return an error.
        msg = '/recovery must be run/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.wiredtiger_open(errdir, self.conn_recerror), msg)

        # If recover=error is run on the directory and returns an error,
        # make sure when we subsequently open with recover=on it properly
        # recovers all the data.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.wiredtiger_open(newdir, self.conn_recerror), msg)

        # Opening with recover=on should succeed.
        self.conn = self.wiredtiger_open(newdir, self.conn_recon)
        # Make sure the data we added originally is there
        self.session = self.setUpSessionOpen(self.conn)
        c = self.session.open_cursor(self.t1, None, None)
        i = 1
        for key, value in c:
            self.assertEqual(i, key)
            self.assertEqual(self.mkvalue(i+1), value)
            i += 1
        self.assertEqual(i, 10001)
        c.close()
        self.close_conn()
        # Reopening with recover=error after a clean shutdown should succeed.
        self.conn = self.wiredtiger_open(newdir, self.conn_recerror)
