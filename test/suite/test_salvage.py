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

import os, struct
from suite_subprocess import suite_subprocess
import wiredtiger, wttest

# test_salvage.py
#    Utilities: wt salvage
class test_salvage(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_salvage.a'
    nentries = 1000
    session_params = 'key_format=S,value_format=S'
    unique = 'SomeUniqueString'

    def populate(self, tablename):
        """
        Insert some simple entries into the table
        """
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        key = ''
        for i in range(0, self.nentries):
            key += str(i)
            if i == self.nentries / 2:
                val = self.unique + '0'
            else:
                val = key + key
            cursor[key] = val
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
            if i == self.nentries / 2:
                wantval = self.unique + '0'
            else:
                wantval = wantkey + wantkey
            self.assertEqual(gotkey, wantkey)
            self.assertTrue(gotval, wantval)
            i += 1
        self.assertEqual(i, self.nentries)
        cursor.close()

    def check_damaged(self, tablename):
        """
        Check a damaged table with a lower standard than check_populate.
        We don't require that all entries are here,
        just that the ones that are here are correct.
        """
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        wantkey = ''
        i = -1
        correct = 0
        for gotkey, gotval in cursor:
            i += 1
            wantkey += str(i)
            if gotkey != wantkey:
                continue
            if i == self.nentries / 2:
                wantval = self.unique + '0'
            else:
                wantval = wantkey + wantkey
            self.assertEqual(gotkey, wantkey)
            self.assertTrue(gotval, wantval)
            correct += 1
        self.assertTrue(correct > 0)
        self.printVerbose(2, 'after salvaging, file has ' + str(correct) + '/' +
                          str(self.nentries) + ' entries')
        cursor.close()

    def check_empty_table(self, tablename):
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        for gotkey, gotval in cursor:
            self.fail(tablename + ': has unexpected entries')
        cursor.close()

    def damage(self, tablename):
        self.damage_inner(tablename, self.unique)

    def damage_inner(self, tablename, unique):
        """
        Open the file for the table, find the unique string
        and modify it.
        """
        self.close_conn()
        # we close the connection to guarantee everything is
        # flushed and closed from the WT point of view.
        filename = tablename + ".wt"

        fp = open(filename, "r+b")
        found = matchpos = 0
        match = unique
        matchlen = len(match)
        flen = os.fstat(fp.fileno()).st_size
        c = fp.read(1)
        while fp.tell() != flen:
            if match[matchpos] == c:
                matchpos += 1
                if matchpos == matchlen:
                    # We're already positioned, so alter it
                    fp.seek(-1, 1)
                    fp.write('G')
                    matchpos = 0
                    found = 1
            else:
                matchpos = 0
            c = fp.read(1)
        # Make sure we found the embedded string
        self.assertTrue(found)
        fp.close()

    def test_salvage_process_empty(self):
        """
        Test salvage in a 'wt' process, using an empty table
        """
        self.session.create('table:' + self.tablename, self.session_params)
        errfile = "salvageerr.out"
        self.runWt(["salvage", self.tablename + ".wt"], errfilename=errfile)
        self.check_empty_file(errfile)
        self.check_empty_table(self.tablename)

    def test_salvage_process(self):
        """
        Test salvage in a 'wt' process, using a populated table.
        """
        self.session.create('table:' + self.tablename, self.session_params)
        self.populate(self.tablename)
        errfile = "salvageerr.out"
        self.runWt(["salvage", self.tablename + ".wt"], errfilename=errfile)
        self.check_empty_file(errfile)
        self.check_populate(self.tablename)

    def test_salvage_api_empty(self):
        """
        Test salvage via API, using an empty table
        """
        self.session.create('table:' + self.tablename, self.session_params)
        self.session.salvage('table:' + self.tablename, None)
        self.check_empty_table(self.tablename)

    def test_salvage_api(self):
        """
        Test salvage via API, using a populated table.
        """
        self.session.create('table:' + self.tablename, self.session_params)
        self.populate(self.tablename)
        self.session.salvage('file:' + self.tablename + ".wt", None)
        self.check_populate(self.tablename)

    def test_salvage_api_damaged(self):
        """
        Test salvage via API, on a damaged table.
        """
        self.session.create('table:' + self.tablename, self.session_params)
        self.populate(self.tablename)
        self.damage(self.tablename)

        # damage() closed the session/connection, reopen them now.
        self.open_conn()
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.verify('table:' + self.tablename, None),
            "/read checksum error/")

        self.session.salvage('file:' + self.tablename + ".wt", None)
        self.check_damaged(self.tablename)

    def test_salvage_process_damaged(self):
        """
        Test salvage in a 'wt' process on a table that is purposely damaged.
        """
        self.session.create('table:' + self.tablename, self.session_params)
        self.populate(self.tablename)
        self.damage(self.tablename)
        errfile = "salvageerr.out"
        self.runWt(["salvage", self.tablename + ".wt"], errfilename=errfile)
        self.check_empty_file(errfile)  # expect no output
        self.check_no_error_in_file(errfile)
        self.check_damaged(self.tablename)

if __name__ == '__main__':
    wttest.run()
