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
# test_txn13.py
# Test very large log records.  Even with a small log file we should be
# able to write them.  Expect an error over 4Gb.
#

#import fnmatch, os, shutil, run, time
from suite_subprocess import suite_subprocess
from wtscenario import check_scenarios
import wiredtiger, wttest

class test_txn13(wttest.WiredTigerTestCase, suite_subprocess):
    logmax = "100K"
    tablename = 'test_txn13'
    uri = 'table:' + tablename
    nops = 1024
    create_params = 'key_format=i,value_format=S'

    scenarios = check_scenarios([
        ('1gb', dict(expect_err=False, valuesize=1048576)),
        ('2gb', dict(expect_err=False, valuesize=2097152)),
        ('4gb', dict(expect_err=True, valuesize=4194304))
    ])

    # Turn on logging for this test.
    def conn_config(self, dir):
        return 'log=(archive=false,enabled,file_max=%s)' % self.logmax + \
            ',cache_size=8G'

    @wttest.longtest('txn tests with huge values')
    def test_large_values(self):
        # print "Creating %s with config '%s'" % (self.uri, self.create_params)
        # print "Running with %d" % (self.valuesize)
        self.session.create(self.uri, self.create_params)
        c = self.session.open_cursor(self.uri, None)

        # We want to test very large values.  Generate 'nops' records within
        # a single transaction.
        valuepfx = self.valuesize * 'X'

        gotException = False
        self.session.begin_transaction()
        for k in range(self.nops):
            value = valuepfx + str(k)
            c[k] = value

        if self.expect_err:
            # EFBIG is expected: File too large
            msg = '/exceeds the maximum/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda:self.session.commit_transaction(), msg)
            gotException = True
        else:
            self.session.commit_transaction()

        self.assertTrue(gotException == self.expect_err)

if __name__ == '__main__':
    wttest.run()
