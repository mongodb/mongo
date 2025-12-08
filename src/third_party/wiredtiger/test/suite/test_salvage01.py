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

# Note that this class is reused by test_encrypt07 test_salvage02; be sure
# to test any changes with that version as well.

class test_salvage01(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_salvage01.a'
    nentries = 1000

    format_values = [
        ('string-row', dict(key_format='S')),
        ('column', dict(key_format='r')),
    ]

    failpoint_enabled = [
        ('fail-point', dict(failpoint=True)),
        ('no-fail-point', dict(failpoint=False)),
    ]

    scenarios = make_scenarios(format_values, failpoint_enabled)

    def conn_config(self):
        if self.failpoint_enabled:
            return 'timing_stress_for_test=[failpoint_eviction_split]'
        else:
            return ''

    def moreinit(self):
        format = 'key_format={},value_format={}'.format(self.key_format, 'S')
        if self.key_format == 'r':
            # VLCS requires smaller pages or the workload fits on one page, which then gets
            # zapped by the damage operation and the table comes out empty, which isn't what
            # we want.
            format += ',leaf_page_max=4096'
        self.session_params = format
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
        if self.key_format == 'r':
            # Use key + i to create gaps.
            return key + i
        else:
            return key + str(i)

    def uniqueval(self):
        return self.unique + '0'

    def ordinaryval(self, key):
        if self.key_format == 'r':
            return str(key) + str(key)
        else:
            return key + key

    def getval(self, entry, key):
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

            wantkey = nextkey
            wantval = self.getval(i, wantkey)

            self.assertEqual(gotkey, wantkey)
            self.assertEqual(gotval, wantval)
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
        self.printVerbose(3, 'after salvaging, file has ' + str(correct) + '/' +
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
        self.ignoreStdoutPatternIfExists("extent list")

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
        self.ignoreStdoutPatternIfExists("extent list")

    def test_salvage_api_empty(self):
        """
        Test salvage via API, using an empty table
        """
        self.moreinit()
        self.session.create('table:' + self.tablename, self.session_params)
        self.session.salvage('table:' + self.tablename, None)
        self.check_empty_table(self.tablename)
        self.ignoreStdoutPatternIfExists("extent list")

    def test_salvage_api(self):
        """
        Test salvage via API, using a populated table.
        """
        self.moreinit()
        self.session.create('table:' + self.tablename, self.session_params)
        self.populate(self.tablename)
        self.salvageUntilSuccess(self.session, 'file:' + self.tablename + ".wt")
        self.check_populate(self.tablename)
        self.ignoreStdoutPatternIfExists("extent list")

    def test_salvage_api_damaged(self):
        """
        Test salvage via API, on a damaged table.
        """
        self.moreinit()
        self.session.create('table:' + self.tablename, self.session_params)
        self.populate(self.tablename)
        self.damage(self.tablename)

        # damage() closed the session/connection, reopen them now.
        # Disable pre-fetching on the connection and its sessions as it is
        # not expected to work correctly with damaged contents.
        #
        # FIXME-WT-12143 - It should be the responsibility of the pre-fetching
        # functionality to stop operating on damaged tables after a restart
        # rather than relying on the user to explicitly turn prefetching off.
        # Revert to opening the connection like so once the fix is implemented:
        # self.open_conn()
        self.open_conn(config='prefetch=(available=false,default=false)')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.verify('table:' + self.tablename, None),
            "/read checksum error/")

        self.session.salvage('file:' + self.tablename + ".wt", None)
        self.check_damaged(self.tablename)
        self.ignoreStdoutPatternIfExists("extent list")

    def test_salvage_process_damaged(self):
        """
        Test salvage in a 'wt' process on a table that is purposely damaged.
        """
        self.moreinit()
        self.session.create('table:' + self.tablename, self.session_params)
        self.populate(self.tablename)
        self.damage(self.tablename)
        errfile = "salvageerr.out"
        outfile = "salvageout.out"
        self.runWt(["salvage", self.tablename + ".wt"], errfilename=errfile, outfilename=outfile)
        self.check_empty_file(errfile)  # expect no output
        self.check_no_error_in_file(errfile)
        self.check_damaged(self.tablename)
        self.ignoreStdoutPatternIfExists("extent list")
