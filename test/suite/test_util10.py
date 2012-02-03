#!/usr/bin/env python
#
# Copyright (c) 2008-2012 WiredTiger, Inc.
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
# test_util10.py
#	Utilities: wt dumpfile
#

import unittest
from wiredtiger import WiredTigerError
import wttest
from suite_subprocess import suite_subprocess
import os
import struct

class test_util10(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_util10.a'
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
            cursor.set_key(key)
            cursor.set_value(val)
            cursor.insert()
        cursor.set_key('SOMEKEY')
        cursor.set_value('SOMEVALUE')
        cursor.insert()
        cursor.set_key('ANOTHERKEY')
        cursor.set_value('ANOTHERVALUE')
        cursor.insert()
        cursor.close()

    def test_dumpfile_empty(self):
        """
        Test read in a 'wt' process, using an empty table
        """
        self.session.create('table:' + self.tablename, self.session_params)
        outfile = "dumpfileout.txt"
        self.runWt(["dumpfile", self.tablename + ".wt"], outfilename=outfile)
        self.check_empty_file(outfile)

    def test_dumpfile_populated(self):
        """
        Test read in a 'wt' process, using an empty table
        """
        self.session.create('table:' + self.tablename, self.session_params)
        self.populate(self.tablename)
        outfile = "dumpfileout.txt"
        self.runWt(["dumpfile", self.tablename + ".wt"], outfilename=outfile)

        # Expected output is roughly K/V pairs in this format:
        #   K {xxxxxx#00}
        #   V {xxxxxx#00}
        # except that by default keys use prefix compression.
        # 'KEY340' would not be found in the output, but rather K {0#00}
        # because it appears immediately after 'KEY34' so uses the five
        # bytes of that key.  We've chosen keys to find that will not be
        # compressed.
        self.check_file_contains(outfile, 'V {VAL22#00}')
        self.check_file_contains(outfile, 'K {KEY0#00}')
        self.check_file_contains(outfile, 'K {SOMEKEY#00}')
        self.check_file_contains(outfile, 'V {SOMEVALUE#00}')
        self.check_file_contains(outfile, 'K {SOMEKEY#00}')
        self.check_file_contains(outfile, 'V {ANOTHERVALUE#00}')


if __name__ == '__main__':
    wttest.run()
