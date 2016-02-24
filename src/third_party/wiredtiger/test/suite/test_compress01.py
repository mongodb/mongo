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
#
# test_compress01.py
#   Basic block compression operations
#

import os, run
import wiredtiger, wttest
from wtscenario import multiply_scenarios, number_scenarios

# Test basic compression
class test_compress01(wttest.WiredTigerTestCase):

    types = [
        ('file', dict(uri='file:test_compress01')),
        ('table', dict(uri='table:test_compress01')),
    ]
    compress = [
        ('nop', dict(compress='nop')),
        ('snappy', dict(compress='snappy')),
        ('none', dict(compress=None)),
    ]
    scenarios = number_scenarios(multiply_scenarios('.', types, compress))

    nrecords = 10000
    bigvalue = "abcdefghij" * 1000

    # Load the compression extension, compression is enabled elsewhere.
    def conn_config(self, dir):
        return self.extensionArg(self.compress)

    # Return the wiredtiger_open extension argument for a shared library.
    def extensionArg(self, name):
        if name == None:
            return ''

        testdir = os.path.dirname(__file__)
        extdir = os.path.join(run.wt_builddir, 'ext/compressors')
        extfile = os.path.join(
            extdir, name, '.libs', 'libwiredtiger_' + name + '.so')
        if not os.path.exists(extfile):
            self.skipTest('compression extension "' + extfile + '" not built')
        return ',extensions=["' + extfile + '"]'

    # Create a table, add keys with both big and small values, then verify them.
    def test_compress(self):

        # Use relatively small leaf pages to force big values to be overflow
        # items, but still large enough that we get some compression action.
        params = 'key_format=S,value_format=S,leaf_page_max=4096'
        if self.compress != None:
            params += ',block_compressor=' + self.compress

        self.session.create(self.uri, params)
        cursor = self.session.open_cursor(self.uri, None)
        for idx in xrange(1,self.nrecords):
            cursor.set_key(`idx`)
            if idx / 12 == 0:
                cursor.set_value(`idx` + self.bigvalue)
            else:
                cursor.set_value(`idx` + "abcdefg")
            cursor.insert()
        cursor.close()

        # Force the cache to disk, so we read compressed pages from disk.
        self.reopen_conn()

        cursor = self.session.open_cursor(self.uri, None)
        for idx in xrange(1,self.nrecords):
            cursor.set_key(`idx`)
            self.assertEqual(cursor.search(), 0)
            if idx / 12 == 0:
                self.assertEquals(cursor.get_value(), `idx` + self.bigvalue)
            else:
                self.assertEquals(cursor.get_value(), `idx` + "abcdefg")
        cursor.close()


if __name__ == '__main__':
    wttest.run()
