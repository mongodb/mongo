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

import os, struct
from suite_subprocess import suite_subprocess
import wiredtiger, wttest

# test_verify.py
#    Utilities: wt verify
class test_verify(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_verify.a'
    nentries = 1000

    def populate(self, tablename):
        """
        Insert some simple entries into the table
        """
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        key = ''
        for i in range(0, self.nentries):
            key += str(i)
            cursor[key] = key + key
        cursor.close()

    def check_populate(self, tablename):
        """
        Verify that items added by populate are still there
        """
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        wantkey = ''
        i = 0
        for gotkey, gotval in cursor:
            wantkey += str(i)
            wantval = wantkey + wantkey
            self.assertEqual(gotkey, wantkey)
            self.assertEqual(gotval, wantval)
            i += 1
        self.assertEqual(i, self.nentries)
        cursor.close()

    def open_and_position(self, tablename, pct):
        """
        Open the file for the table, position it at a 4K page
        at roughly the given percentage into the file.
        As a side effect, the connection is closed.
        """
        # we close the connection to guarantee everything is
        # flushed and closed from the WT point of view.
        if self.conn != None:
            self.conn.close()
            self.conn = None
        filename = tablename + ".wt"

        filesize = os.path.getsize(filename)
        position = (filesize * pct) // 100

        self.pr('damaging file at: ' + str(position))
        fp = open(filename, "r+b")
        fp.seek(position)
        return fp

    def test_verify_process_empty(self):
        """
        Test verify in a 'wt' process, using an empty table
        """
        params = 'key_format=S,value_format=S'
        self.session.create('table:' + self.tablename, params)
        # Run verify with an empty table
        self.runWt(["verify", "table:" + self.tablename])

    def test_verify_process(self):
        """
        Test verify in a 'wt' process, using a populated table.
        """
        params = 'key_format=S,value_format=S'
        self.session.create('table:' + self.tablename, params)
        self.populate(self.tablename)
        self.runWt(["verify", "table:" + self.tablename])

    def test_verify_api_empty(self):
        """
        Test verify via API, using an empty table
        """
        params = 'key_format=S,value_format=S'
        self.session.create('table:' + self.tablename, params)
        self.session.verify('table:' + self.tablename, None)

    def test_verify_api(self):
        """
        Test verify via API, using a populated table.
        """
        params = 'key_format=S,value_format=S'
        self.session.create('table:' + self.tablename, params)
        self.populate(self.tablename)
        self.session.verify('table:' + self.tablename, None)
        self.check_populate(self.tablename)

    def test_verify_api_75pct_null(self):
        """
        Test verify via API, on a damaged table.
        This is our only 'negative' test for verify using the API,
        it's uncertain that we can have reliable tests for this.
        """
        params = 'key_format=S,value_format=S'
        self.session.create('table:' + self.tablename, params)
        self.populate(self.tablename)
        with self.open_and_position(self.tablename, 75) as f:
            for i in range(0, 4096):
                f.write(struct.pack('B', 0))

        # open_and_position closed the session/connection, reopen them now.
        self.conn = self.setUpConnectionOpen(".")
        self.session = self.setUpSessionOpen(self.conn)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.verify('table:' + self.tablename, None),
            "/WT_SESSION.verify/")

    def test_verify_process_75pct_null(self):
        """
        Test verify in a 'wt' process on a table that is purposely damaged,
        with nulls at a position about 75% through.
        """
        params = 'key_format=S,value_format=S'
        self.session.create('table:' + self.tablename, params)
        self.populate(self.tablename)
        with self.open_and_position(self.tablename, 75) as f:
            for i in range(0, 4096):
                f.write(struct.pack('B', 0))
        self.runWt(["verify", "table:" + self.tablename],
            errfilename="verifyerr.out", failure=True)
        self.check_non_empty_file("verifyerr.out")

    def test_verify_process_25pct_junk(self):
        """
        Test verify in a 'wt' process on a table that is purposely damaged,
        with junk at a position about 25% through.
        """
        params = 'key_format=S,value_format=S'
        self.session.create('table:' + self.tablename, params)
        self.populate(self.tablename)
        with self.open_and_position(self.tablename, 25) as f:
            for i in range(0, 100):
                f.write(b'\x01\xff\x80')
        self.runWt(["verify", "table:" + self.tablename],
            errfilename="verifyerr.out", failure=True)
        self.check_non_empty_file("verifyerr.out")

    def test_verify_process_truncated(self):
        """
        Test verify in a 'wt' process on a table that is purposely damaged,
        truncated about 75% through.
        """
        params = 'key_format=S,value_format=S'
        self.session.create('table:' + self.tablename, params)
        self.populate(self.tablename)
        with self.open_and_position(self.tablename, 75) as f:
            f.truncate(0)
        self.runWt(["verify", "table:" + self.tablename],
            errfilename="verifyerr.out", failure=True)
        # The test may output the following error message while opening a file that
        # does not exist. Ignore that.
        self.ignoreStderrPatternIfExists('No such file or directory')

    def test_verify_process_zero_length(self):
        """
        Test verify in a 'wt' process on a zero-length table.
        """
        params = 'key_format=S,value_format=S'
        self.session.create('table:' + self.tablename, params)
        self.populate(self.tablename)
        with self.open_and_position(self.tablename, 0) as f:
            f.truncate(0)
        self.runWt(["verify", "table:" + self.tablename],
            errfilename="verifyerr.out", failure=True)
        # The test may output the following error message while opening a file that
        # does not exist. Ignore that.
        self.ignoreStderrPatternIfExists('No such file or directory')

if __name__ == '__main__':
    wttest.run()
