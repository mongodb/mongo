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
# [TEST_TAGS]
# wt_util
# [END_TAGS]

from suite_subprocess import suite_subprocess
import wttest

# test_util11.py
#    Utilities: wt list
class test_util11(wttest.WiredTigerTestCase, suite_subprocess):
    tablenamepfx = 'test_util11.'
    session_params = 'key_format=S,value_format=S'

    def populate(self, tablename):
        """
        Insert some simple entries into the table
        """
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        cursor['SOMEKEY'] = 'SOMEVALUE'
        cursor.close()

    def create_tables(self, tablename):
        """
        Create a mix of populated and empty tables
        """
        params = self.session_params
        self.session.create('table:' + tablename + '5', params)
        self.session.create('table:' + tablename + '3', params)
        self.session.create('table:' + tablename + '1', params)
        self.session.create('table:' + tablename + '2', params)
        self.session.create('table:' + tablename + '4', params)
        self.populate(tablename + '2')
        self.populate(tablename + '3')

    def compare_two_files(self, file1, file2):
        """
        Return true if files contain same content, false otherwise
        """
        try:
            with open(file1, 'rb') as f1, open(file2, 'rb') as f2:
                return f1.read() == f2.read()
        except FileNotFoundError:
            return False

    def test_list_none(self):
        """
        Test list in a 'wt' process, with no tables
        """

        # Construct what we think we'll find
        filelist = ''
        outfile = "listout.txt"
        self.runWt(["list"], outfilename=outfile)
        self.check_file_content(outfile, filelist)

    def test_list(self):
        """
        Test list in a 'wt' process, with a mix of populated and empty tables
        """
        pfx = self.tablenamepfx
        self.create_tables(pfx)

        # Construct what we think we'll find
        tablelist = ''
        for i in range(1, 6):
            tablelist += 'table:' + pfx + str(i) + '\n'

        outfile = "listout.txt"
        self.runWt(["list", "table:"], outfilename=outfile)
        self.check_file_content(outfile, tablelist)

    def test_list_drop(self):
        """
        Test list in a 'wt' process, with a mix of populated and empty tables,
        after some tables have been dropped.
        """
        pfx = self.tablenamepfx
        self.create_tables(pfx)

        self.dropUntilSuccess(self.session, 'table:' + pfx + '2')
        self.dropUntilSuccess(self.session, 'table:' + pfx + '4')

        # Construct what we think we'll find
        tablelist = ''.join('table:' + pfx + str(i) + '\n' for i in (1, 3, 5))

        outfile = "listout.txt"
        self.runWt(["list", "table:"], outfilename=outfile)
        self.check_file_content(outfile, tablelist)

    def test_list_drop_all(self):
        """
        Test list in a 'wt' process, with a mix of populated and empty tables,
        after all tables have been dropped.
        """
        pfx = self.tablenamepfx
        self.create_tables(pfx)

        self.dropUntilSuccess(self.session, 'table:' + pfx + '5')
        self.dropUntilSuccess(self.session, 'table:' + pfx + '4')
        self.dropUntilSuccess(self.session, 'table:' + pfx + '3')
        self.dropUntilSuccess(self.session, 'table:' + pfx + '2')
        self.dropUntilSuccess(self.session, 'table:' + pfx + '1')

        # Construct what we think we'll find
        filelist = ''
        outfile = "listout.txt"
        self.runWt(["list"], outfilename=outfile)
        self.check_file_content(outfile, filelist)

    def test_list_file_output(self):
        """
        Test list in a 'wt' process, checking custom output file content
        """
        pfx = self.tablenamepfx
        self.create_tables(pfx)

        outfile = "listout.txt"
        customfile = "listcustomout.txt"
        self.runWt(["list", "-c", "-f", customfile, "table:"], outfilename=outfile)

        # Check that stdout is empty
        filelist = ''
        self.check_file_content(outfile, filelist)

        # Run without custom output file argument
        self.runWt(["list", "-c", "table:"], outfilename=outfile)

        # Compare output with and without -f
        self.compare_two_files(customfile, outfile)
