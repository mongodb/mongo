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
import wiredtiger, wttest

# test_util12.py
#    Utilities: wt write
class test_util12(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_util12.a'
    session_params = 'key_format=S,value_format=S'
    errfile = 'writeerr.txt'

    def test_write(self):
        """
        Test write in a 'wt' process, without command option
        """
        self.session.create('table:' + self.tablename, self.session_params)
        self.runWt(['write', 'table:' + self.tablename,
                    'def', '456', 'abc', '123'])
        cursor = self.session.open_cursor('table:' + self.tablename, None, None)
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), 'abc')
        self.assertEqual(cursor.get_value(), '123')
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), 'def')
        self.assertEqual(cursor.get_value(), '456')
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
        cursor.close()

    def test_write_overwrite(self):
        """
        Test write in a 'wt' process, with the '-o' command option
        """
        self.session.create('table:' + self.tablename, self.session_params)
        cursor = self.session.open_cursor('table:' + self.tablename, None, None)
        cursor['def'] = '789'
        cursor.close()
        # The below command is expected to fail as we attempt to overwrite an existing key
        # without specifying the "-o" command option.
        self.runWt(['write', 'table:' + self.tablename,
                    'def', '456', 'abc', '123'], errfilename=self.errfile, failure=True)
        self.check_file_contains(self.errfile, 'attempt to insert an existing key')
        self.runWt(['write', '-o', 'table:' + self.tablename,
                    'def', '456', 'abc', '123'])
        cursor = self.session.open_cursor('table:' + self.tablename, None, None)
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), 'abc')
        self.assertEqual(cursor.get_value(), '123')
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), 'def')
        self.assertEqual(cursor.get_value(), '456')
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
        cursor.close()

    def test_write_remove(self):
        """
        Test write in a 'wt' process, with the '-r' command option
        """
        self.session.create('table:' + self.tablename, self.session_params)
        self.runWt(['write', 'table:' + self.tablename,
                    'def', '456', 'abc', '123'])
        self.runWt(['write', '-r', 'table:' + self.tablename, 'efg'],
            errfilename=self.errfile, failure=True)
        self.check_file_contains(self.errfile, 'item not found')
        # The below command is expected to fail as more than one key are specified.
        self.runWt(['write', '-r', 'table:' + self.tablename, 'def', 'abc'],
            errfilename=self.errfile, failure=True)
        self.check_file_contains(self.errfile, 'usage:')
        self.runWt(['write', '-r', 'table:' + self.tablename, 'def'])
        cursor = self.session.open_cursor('table:' + self.tablename, None, None)
        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), 'abc')
        self.assertEqual(cursor.get_value(), '123')
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
        cursor.close()

    def test_write_no_keys(self):
        """
        Test write in a 'wt' process, with no command args
        """
        self.session.create('table:' + self.tablename, self.session_params)

        self.runWt(['write', 'table:' + self.tablename],
            errfilename=self.errfile, failure=True)
        self.check_file_contains(self.errfile, 'usage:')

    def test_write_bad_args(self):
        """
        Test write in a 'wt' process, with unexpected number of args
        """
        self.session.create('table:' + self.tablename, self.session_params)
        # The below command is expected to fail as value for the 2nd key is missed.
        self.runWt(['write', 'table:' + self.tablename,
                    'def', '456', 'abc'], errfilename=self.errfile, failure=True)
        self.check_file_contains(self.errfile, 'usage:')

if __name__ == '__main__':
    wttest.run()
