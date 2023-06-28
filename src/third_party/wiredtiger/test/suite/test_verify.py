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

    # Returns the .wt file extension, or in the case
    # of tiered storage, builds the .wtobj object name.
    # Assumes that no checkpoints are done, so we
    # are on the first object.
    def file_name(self, name):
        return self.initialFileName('table:' + name)

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

    def count_file_contains(self, filename, content):
        count = 0
        with open(filename) as f:
            for line in f:
                if content in line:
                    count += 1
            f.close()
        return count

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
        filename = self.file_name(tablename)

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
        self.verifyUntilSuccess(self.session, 'table:' + self.tablename)
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
            lambda: self.session.verify('table:' + self.tablename, "read_corrupt"),
            "/WT_SESSION.verify/")
        self.assertEqual(self.count_file_contains("stderr.txt",
            "calculated block checksum of"), 1)

    def test_verify_api_read_corrupt_pages(self):
        """
        Test verify via API, on a table that is purposely corrupted in
        multiple places. A verify operation with read_corrupt on should
        result in multiple checksum errors being logged.
        """
        params = 'key_format=S,value_format=S'
        self.session.create('table:' + self.tablename, params)
        self.populate(self.tablename)
        with self.open_and_position(self.tablename, 25) as f:
            for i in range(0, 100):
                f.write(b'\x01\xff\x80')
        with self.open_and_position(self.tablename, 50) as f:
            for i in range(0, 100):
                f.write(b'\x01\xff\x80')
        with self.open_and_position(self.tablename, 75) as f:
            for i in range(0, 100):
                f.write(b'\x01\xff\x80')

        # open_and_position closed the session/connection, reopen them now.
        self.conn = self.setUpConnectionOpen(".")
        self.session = self.setUpSessionOpen(self.conn)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.verify('table:' + self.tablename, "read_corrupt"),
            "/WT_SESSION.verify/")

        # It is expected that more than one checksum error is logged given
        # that we have corrupted the table in multiple locations, but we may
        # not necessarily detect all three corruptions - e.g. we won't detect
        # a corruption if we overwrite free space or overwrite a page that is
        # a child of another page that we overwrite.
        self.assertGreater(self.count_file_contains("stderr.txt",
            "calculated block checksum of"), 1)

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
        self.runWt(["verify", "-c", "table:" + self.tablename],
            errfilename="verifyerr.out", failure=True)
        self.check_non_empty_file("verifyerr.out")
        self.assertEqual(self.count_file_contains("verifyerr.out",
            "calculated block checksum of"), 1)

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
        self.runWt(["verify", "-c", "table:" + self.tablename],
            errfilename="verifyerr.out", failure=True)
        self.check_non_empty_file("verifyerr.out")
        self.assertEqual(self.count_file_contains("verifyerr.out",
            "calculated block checksum of"), 1)

    def test_verify_process_read_corrupt_pages(self):
        """
        Test verify in a 'wt' process on a table that is purposely corrupted
        in multiple places. A verify operation with read_corrupt on should
        result in multiple checksum errors being logged.
        """
        params = 'key_format=S,value_format=S'
        self.session.create('table:' + self.tablename, params)
        self.populate(self.tablename)
        with self.open_and_position(self.tablename, 25) as f:
            for i in range(0, 100):
                f.write(b'\x01\xff\x80')
        with self.open_and_position(self.tablename, 75) as f:
            for i in range(0, 100):
                f.write(b'\x01\xff\x80')
        with self.open_and_position(self.tablename, 80) as f:
            for i in range(0, 100):
                f.write(b'\x01\xff\x80')
        self.runWt(["verify", "-c", "table:" + self.tablename],
            errfilename="verifyerr.out", failure=True)

        self.check_non_empty_file("verifyerr.out")

        # It is expected that more than one checksum error is logged given
        # that we have corrupted the table in multiple locations, but we may
        # not necessarily detect all three corruptions - e.g. we won't detect
        # a corruption if we overwrite free space or overwrite a page that is
        # a child of another page that we overwrite.
        self.assertGreater(self.count_file_contains("verifyerr.out",
            "calculated block checksum of"), 1)

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

    def test_verify_all(self):
        """
        Test verify in a 'wt' process without a specific table URI argument.
        """
        params = 'key_format=S,value_format=S'
        ntables = 3

        for i in range(ntables):
            self.session.create('table:' + self.tablename + str(i), params)
            self.populate(self.tablename + str(i))
        self.session.checkpoint()

        self.runWt(["verify"])

        # Purposely corrupt the last two tables. Test that verifying the database
        # with the abort option stops after seeing the first corrupted table.
        for i in range(1, ntables):
            with self.open_and_position(self.tablename + str(i), 75) as f:
                for i in range(0, 4096):
                    f.write(struct.pack('B', 0))

        self.runWt(["verify", "-a"], errfilename="verifyerr.out", failure=True)
        self.assertEqual(self.count_file_contains("verifyerr.out",
            "table:test_verify.a1: WT_ERROR"), 1)
        self.assertEqual(self.count_file_contains("verifyerr.out",
            "table:test_verify.a2: WT_ERROR"), 0)

if __name__ == '__main__':
    wttest.run()
