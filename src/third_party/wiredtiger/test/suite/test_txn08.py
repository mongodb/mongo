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
# test_txn08.py
# Printlog: test Unicode output
#

from suite_subprocess import suite_subprocess
import wttest
from wtscenario import make_scenarios

class test_txn08(wttest.WiredTigerTestCase, suite_subprocess):
    logmax = "100K"
    tablename = 'test_txn08'
    uri = 'table:' + tablename

    key_format_values = [
        ('col', dict(key_format='r')),
        ('row', dict(key_format='i'))
    ]
    scenarios = make_scenarios(key_format_values)

    # Turn on logging for this test.
    def conn_config(self):
        return 'log=(enabled,file_max=%s,remove=false),' % self.logmax + \
            'transaction_sync="(method=dsync,enabled)"'

    def test_printlog_unicode(self):
        create_params = 'key_format={},value_format=S'.format(self.key_format)
        # print "Creating %s with config '%s'" % (self.uri, create_params)
        self.session.create(self.uri, create_params)
        c = self.session.open_cursor(self.uri, None)

        # We want to test some chars that produce Unicode encoding
        # for printlog output.
        value = u'\u0001\u0002abcd\u0003\u0004'

        self.session.begin_transaction()
        for k in range(1, 6):
            c[k] = value

        self.session.commit_transaction()

        #
        # Run printlog and make sure it exits with zero status.
        #
        self.runWt(['printlog', '-u'], outfilename='printlog.out')
        self.check_file_contains('printlog.out',
            '\\u0001\\u0002abcd\\u0003\\u0004')
        self.runWt(['printlog', '-u','-x'], outfilename='printlog-hex.out')
        self.check_file_contains('printlog-hex.out',
            '\\u0001\\u0002abcd\\u0003\\u0004')
        self.check_file_contains('printlog-hex.out',
            '0102616263640304')
        # Check the printlog start LSN and stop LSN feature.
        self.runWt(['printlog', '-l 2,128'], outfilename='printlog-range01.out')
        self.check_file_contains('printlog-range01.out',
            '"lsn" : [2,128],')
        self.check_file_contains('printlog-range01.out',
            '"lsn" : [2,256],')
        self.check_file_not_contains('printlog-range01.out',
            '"lsn" : [1,128],')
        self.runWt(['printlog', '-l 2,128,3,128'], outfilename='printlog-range02.out')
        self.check_file_contains('printlog-range02.out',
            '"lsn" : [2,128],')
        self.check_file_not_contains('printlog-range02.out',
            '"lsn" : [1,128],')
        self.check_file_not_contains('printlog-range02.out',
            '"lsn" : [3,256],')
        # Test for invalid LSN, return WT_NOTFOUND
        self.runWt(['printlog', '-l 2,300'], outfilename='printlog-range03.out', errfilename='printlog-range03.err', failure=True)
        self.check_file_contains('printlog-range03.err','WT_NOTFOUND')
        # Test for Start > end, print the start lsn and then stop
        self.runWt(['printlog', '-l 3,128,2,128'], outfilename='printlog-range04.out')
        self.check_file_contains('printlog-range04.out','"lsn" : [3,128],')
        self.check_file_not_contains('printlog-range04.out','"lsn" : [3,256],')
        # Test for usage error, print the usage message if arguments are invalid
        self.runWt(['printlog', '-l'], outfilename='printlog-range05.out', errfilename='printlog-range05.err', failure=True)
        self.check_file_contains('printlog-range05.err','wt: option requires an argument -- l')
        # Test start and end offset of 0
        self.runWt(['printlog', '-l 2,0,3,0'], outfilename='printlog-range06.out')
        self.check_file_contains('printlog-range06.out',
            '"lsn" : [2,128],')
        self.check_file_not_contains('printlog-range06.out',
            '"lsn" : [1,128],')
        self.check_file_not_contains('printlog-range06.out',
            '"lsn" : [3,256],')
        # Test for start == end
        self.runWt(['printlog', '-l 1,256,1,256'], outfilename='printlog-range07.out')
        self.check_file_contains('printlog-range07.out',
            '"lsn" : [1,256],')
        self.check_file_not_contains('printlog-range07.out',
            '"lsn" : [1,128],')
        self.check_file_not_contains('printlog-range07.out',
            '"lsn" : [1,384],')

if __name__ == '__main__':
    wttest.run()
