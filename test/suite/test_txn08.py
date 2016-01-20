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
# test_txn08.py
# Printlog: test Unicode output
#

import fnmatch, os, shutil, run, time
from suite_subprocess import suite_subprocess
from wiredtiger import stat
from wtscenario import multiply_scenarios, number_scenarios
import wttest

class test_txn08(wttest.WiredTigerTestCase, suite_subprocess):
    logmax = "100K"
    tablename = 'test_txn08'
    uri = 'table:' + tablename

    # Turn on logging for this test.
    def conn_config(self, dir):
        return 'log=(archive=false,enabled,file_max=%s),' % self.logmax + \
            'transaction_sync="(method=dsync,enabled)"'

    def test_printlog_unicode(self):
        # print "Creating %s with config '%s'" % (self.uri, self.create_params)
        create_params = 'key_format=i,value_format=S'
        self.session.create(self.uri, create_params)
        c = self.session.open_cursor(self.uri, None)

        # We want to test some chars that produce Unicode encoding
        # for printlog output.
        value = u'\u0001\u0002abcd\u0003\u0004'

        self.session.begin_transaction()
        for k in range(5):
            c[k] = value

        self.session.commit_transaction()

        #
        # Run printlog and make sure it exits with zero status.
        #
        self.runWt(['printlog'], outfilename='printlog.out')
        self.check_file_contains('printlog.out',
            '\\u0001\\u0002abcd\\u0003\\u0004')
        self.runWt(['printlog', '-x'], outfilename='printlog-hex.out')
        self.check_file_contains('printlog-hex.out',
            '\\u0001\\u0002abcd\\u0003\\u0004')
        self.check_file_contains('printlog-hex.out',
            '0102616263640304')

if __name__ == '__main__':
    wttest.run()
