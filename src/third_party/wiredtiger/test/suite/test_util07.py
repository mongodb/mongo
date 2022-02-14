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

from suite_subprocess import suite_subprocess
import wttest

# test_util07.py
#    Utilities: wt read
class test_util07(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_util07.a'
    nentries = 1000
    session_params = 'key_format=S,value_format=S'

    def populate(self, tablename):
        """
        Insert some simple entries into the table
        """
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        for i in range(0, self.nentries):
            key = 'KEY' + str(i)
            val = 'VAL' + str(i)
            cursor[key] = val
        cursor.close()

    def close_conn(self):
        """
        Close the connection if already open.
        """
        if self.conn != None:
            self.conn.close()
            self.conn = None

    def open_conn(self):
        """
        Open the connection if already closed.
        """
        if self.conn == None:
            self.conn = self.setUpConnectionOpen(".")
            self.session = self.setUpSessionOpen(self.conn)

    def test_read_empty(self):
        """
        Test read in a 'wt' process, using an empty table
        """
        self.session.create('table:' + self.tablename, self.session_params)
        outfile = "readout.txt"
        errfile = "readerr.txt"
        self.runWt(["read", 'table:' + self.tablename, 'NoMatch'],
            outfilename=outfile, errfilename=errfile, failure=True)
        self.check_empty_file(outfile)
        self.check_file_contains(errfile, 'NoMatch: not found\n')

    def test_read_populated(self):
        """
        Test read in a 'wt' process, using an empty table
        """
        self.session.create('table:' + self.tablename, self.session_params)
        self.populate(self.tablename)
        outfile = "readout.txt"
        errfile = "readerr.txt"
        self.runWt(["read", 'table:' + self.tablename, 'KEY49'],
            outfilename=outfile, errfilename=errfile)
        self.check_file_content(outfile, 'VAL49\n')
        self.check_empty_file(errfile)
        self.runWt(["read", 'table:' + self.tablename, 'key49'],
            outfilename=outfile, errfilename=errfile, failure=True)
        self.check_empty_file(outfile)
        self.check_file_contains(errfile, 'key49: not found\n')

if __name__ == '__main__':
    wttest.run()
