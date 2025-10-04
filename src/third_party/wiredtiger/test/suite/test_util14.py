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
import wttest

# test_util14.py
#    Utilities: wt truncate
class test_util14(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_util14.a'
    nentries = 1000

    def test_truncate_process(self):
        """
        Test truncate in a 'wt' process
        """
        params = 'key_format=S,value_format=S'
        self.session.create('table:' + self.tablename, params)
        self.assertTrue(self.tableExists(self.tablename))
        cursor = self.session.open_cursor('table:' + self.tablename, None, None)
        for i in range(0, self.nentries):
            cursor[str(i)] = str(i)
        cursor.close()

        self.runWt(["truncate", "table:" + self.tablename])

        """
        Test to confirm table exists and is empty
        """
        outfile="outfile.txt"
        errfile="errfile.txt"
        self.assertTrue(self.tableExists(self.tablename))
        self.runWt(["read", 'table:' + self.tablename, 'NoMatch'],
            outfilename=outfile, errfilename=errfile, failure=True)
        self.check_empty_file(outfile)
        self.check_file_contains(errfile, 'NoMatch: not found\n')

        """
        Tests for error cases
        1. Missing URI
        2. Invalid URI
        3. Valid but incorrect URI
        4. Double URI
        """
        self.runWt(["truncate"],
            outfilename=outfile, errfilename=errfile, failure=True)
        self.check_empty_file(outfile)
        self.check_file_contains(errfile, 'usage:')

        self.runWt(["truncate", "foobar"],
            outfilename=outfile, errfilename=errfile, failure=True)
        self.check_empty_file(outfile)
        self.check_file_contains(errfile, 'No such file or directory')

        self.runWt(["truncate", 'table:xx' + self.tablename],
            outfilename=outfile, errfilename=errfile, failure=True)
        self.check_empty_file(outfile)
        self.check_file_contains(errfile, 'No such file or directory')

        self.runWt(["truncate", 'table:' + self.tablename, 'table:' + self.tablename],
            outfilename=outfile, errfilename=errfile, failure=True)
        self.check_empty_file(outfile)
        self.check_file_contains(errfile, 'usage:')
