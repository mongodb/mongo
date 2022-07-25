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

import os
from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wtscenario import make_scenarios

# test_salvage01.py
#    Utilities: wt salvage

# Note that this class is reused by test_encrypt07; be sure to test any changes with
# that version as well.

class test_salvage(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_salvage01.a'
    nentries = 1000

    format_values = [
        ('string-row', dict(key_format='S', value_format='S')),
        ('column', dict(key_format='r', value_format='S')),
        ('column-fix', dict(key_format='r', value_format='8t')),
    ]
    scenarios = make_scenarios(format_values)

    def moreinit(self):
        format = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        if self.key_format == 'r':
            # VLCS requires smaller pages or the workload fits on one page, which then gets
            # zapped by the damage operation and the table comes out empty, which isn't what
            # we want.
            format += ',leaf_page_max=4096'
        self.session_params = format

        if self.value_format == '8t':
            # For FLCS, we write out a sequence of known bytes in adjacent rows and search for
            # that sequence when we want to corrupt the table. This eliminates the problem
            # earlier versions of this test had where it was looking for a single row value,
            # namely a single byte, and would sometimes find it in unintended places, such as
            # the root page, resulting in unsalvageable corruption and test failure. However,
            # it's still possible for the test to fail arbitrarily if the target sequence ends
            # up spanning a page boundary. Make it only 8 bytes long to reduce the chance of
            # this while still making false matches unlikely. If the test starts failing for
            # no obvious reason, try changing uniquepos by offsets of 8; if that fixes it, the
            # problem is almost certainly this (accidentally spanning a page boundary) and not
            # a real bug.

            # This spells '<taRGet>', and in hex is: 3c 74 61 52 47 65 74 3e.
            # Note that it should not be changed to contain values >= 127 or the Python trip
            # through UTF-8 will break things.
            self.unique = [92, 53, 34, 114, 103, 38, 53, 94]
            self.uniquelen = len(self.unique)
            self.uniquebytes = bytes(self.unique)

        else:
            # For VLCS and row-store, write out a single known value in one row as the target
            # for intentional corruption. Don't set self.uniquelen; nothing should refer to it.
            # (But if something needs to in the future, the correct value is 1.)
            self.unique = 'SomeUniqueString'
            self.uniquebytes = self.unique.encode()

        self.uniquepos = self.nentries // 2

    def firstkey(self):
        if self.key_format == 'r':
            return 1
        return ''

    def nextkey(self, key, i):
        if self.value_format == '8t':
            # Use key + i to create gaps. This makes the actual number of rows larger, but
            # since the rows are small that doesn't make the table excessively large. Don't
            # insert gaps between the bytes in the match string, though, since that makes it
            # excessively likely that the match string spans pages and then we won't be able
            # to find it.
            if i >= self.uniquepos and i <= self.uniquepos + self.uniquelen:
                return key + 1
            else:
                return key + i
        elif self.key_format == 'r':
            # Use key + i to create gaps.
            return key + i
        else:
            return key + str(i)

    def uniqueval(self):
        if self.value_format == '8t':
            # Note that this is a list.
            return self.unique
        else:
            return self.unique + '0'

    def ordinaryval(self, key):
        if self.value_format == '8t':
            return key % 181 + 33
        elif self.key_format == 'r':
            return str(key) + str(key)
        else:
            return key + key

    def getval(self, entry, key):
        if self.value_format == '8t':
            if entry >= self.uniquepos and entry < self.uniquepos + self.uniquelen:
                # Get the proper byte from the corruption target string.
                val = self.uniqueval()
                val = val[entry - self.uniquepos]
            else:
                val = self.ordinaryval(key)
        else:
            if entry == self.uniquepos:
                # Get the corruption target string.
                val = self.uniqueval()
            else:
                val = self.ordinaryval(key)
        return val

    def populate(self, tablename):
        """
        Insert some simple entries into the table
        """
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        key = self.firstkey()
        for i in range(0, self.nentries):
            key = self.nextkey(key, i)
            cursor[key] = self.getval(i, key)
        cursor.close()

    def check_populate(self, tablename):
        """
        Verify that items added by populate are still there
        """
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        wantkey = self.firstkey()
        i = 0
        zeros = 0
        for gotkey, gotval in cursor:
            nextkey = self.nextkey(wantkey, i)

            # In FLCS the values between the keys we wrote will read as zero. Count them.
            if gotkey < nextkey and self.value_format == '8t':
                self.assertEqual(gotval, 0)
                zeros += 1
                continue

            wantkey = nextkey
            wantval = self.getval(i, wantkey)

            self.assertEqual(gotkey, wantkey)
            self.assertEqual(gotval, wantval)
            i += 1
        self.assertEqual(i, self.nentries)
        if self.value_format == '8t':
            # We should have visited every key, so the total row count should match the last key.
            self.assertEqual(self.nentries + zeros, wantkey)
        cursor.close()

    def check_damaged(self, tablename):
        """
        Check a damaged table with a lower standard than check_populate.
        We don't require that all entries are here,
        just that the ones that are here are correct.
        """
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        wantkey = self.firstkey()
        i = -1
        correct = 0
        for gotkey, gotval in cursor:
            i += 1
            wantkey = self.nextkey(wantkey, i)
            if gotkey != wantkey:
                # Note that if a chunk in the middle of the table got lost,
                # this will never sync up again.
                continue
            wantval = self.getval(i, wantkey)
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
        self.damage_inner(tablename, self.uniquebytes)

    def read_byte(self, fp):
        """
        Return a single byte from a file opened in binary mode.
        """
        return fp.read(1)[0]

    def damage_inner(self, tablename, unique):
        """
        Open the file for the table, find the unique string
        and modify it.
        """
        self.close_conn()
        self.assertTrue(type(unique) == bytes)
        # we close the connection to guarantee everything is
        # flushed and closed from the WT point of view.
        filename = tablename + ".wt"

        fp = open(filename, "r+b")
        found = matchpos = 0
        match = unique
        matchlen = len(match)
        flen = os.fstat(fp.fileno()).st_size
        c = self.read_byte(fp)
        while fp.tell() != flen:
            if match[matchpos] == c:
                matchpos += 1
                if matchpos == matchlen:
                    # We're already positioned, so alter it
                    fp.seek(-1, 1)
                    fp.write(b'G')
                    matchpos = 0
                    found = 1
            else:
                matchpos = 0
            c = self.read_byte(fp)
        # Make sure we found the embedded string
        self.assertTrue(found)
        fp.close()

    def test_salvage_process_empty(self):
        """
        Test salvage in a 'wt' process, using an empty table
        """
        self.moreinit()
        self.session.create('table:' + self.tablename, self.session_params)
        errfile = "salvageerr.out"
        self.runWt(["salvage", self.tablename + ".wt"], errfilename=errfile)
        self.check_empty_file(errfile)
        self.check_empty_table(self.tablename)

    def test_salvage_process(self):
        """
        Test salvage in a 'wt' process, using a populated table.
        """
        self.moreinit()
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
        self.moreinit()
        self.session.create('table:' + self.tablename, self.session_params)
        self.session.salvage('table:' + self.tablename, None)
        self.check_empty_table(self.tablename)

    def test_salvage_api(self):
        """
        Test salvage via API, using a populated table.
        """
        self.moreinit()
        self.session.create('table:' + self.tablename, self.session_params)
        self.populate(self.tablename)
        self.salvageUntilSuccess(self.session, 'file:' + self.tablename + ".wt")
        self.check_populate(self.tablename)

    def test_salvage_api_damaged(self):
        """
        Test salvage via API, on a damaged table.
        """
        self.moreinit()
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
        self.moreinit()
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
