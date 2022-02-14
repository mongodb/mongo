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

# test_util09.py
#    Utilities: wt loadtext
class test_util09(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_util09.a'
    nentries = 1000
    session_params = 'key_format=S,value_format=S'

    def populate_file(self, filename, low, high):
        """
        Insert some simple key // value lines into the file
        """
        keys = {}
        with open("loadtext.in", "w") as f:
            for i in range(low, high):
                key = str(i) + str(i)
                val = key + key + key
                f.write(key + '\n')
                f.write(val + '\n')
                keys[key] = val
        #print 'Populated ' + str(len(keys))
        return keys

    def check_keys(self, tablename, keys):
        """
        Check that all the values in the table match the saved dictionary.
        Values in the dictionary are removed as a side effect.
        """
        cursor = self.session.open_cursor('table:' + tablename, None, None)
        for key, val in cursor:
            self.assertEqual(keys[key], val)
            del keys[key]
        cursor.close()
        self.assertEqual(len(keys), 0)

    def test_loadtext_empty(self):
        """
        Test loadtext in a 'wt' process, using an empty table
        """
        self.session.create('table:' + self.tablename, self.session_params)
        keys = self.populate_file("loadtext.in", 0, 0)
        self.runWt(["loadtext", "-f", "loadtext.in", "table:" + self.tablename])
        self.check_keys(self.tablename, keys)

    def test_loadtext_empty_stdin(self):
        """
        Test loadtext in a 'wt' process using stdin, using an empty table
        """
        self.session.create('table:' + self.tablename, self.session_params)
        keys = self.populate_file("loadtext.in", 0, 0)
        self.runWt(["loadtext", "table:" + self.tablename], infilename="loadtext.in")
        self.check_keys(self.tablename, keys)

    def test_loadtext_populated(self):
        """
        Test loadtext in a 'wt' process, creating entries in a table
        """
        self.session.create('table:' + self.tablename, self.session_params)
        keys = self.populate_file("loadtext.in", 1010, 1220)
        self.runWt(["loadtext", "-f", "loadtext.in", "table:" + self.tablename])
        self.check_keys(self.tablename, keys)

    def test_loadtext_populated_stdin(self):
        """
        Test loadtext in a 'wt' process using stding, creating entries in a table
        """
        self.session.create('table:' + self.tablename, self.session_params)
        keys = self.populate_file("loadtext.in", 200, 300)
        self.runWt(["loadtext", "table:" + self.tablename], infilename="loadtext.in")
        self.check_keys(self.tablename, keys)

if __name__ == '__main__':
    wttest.run()
