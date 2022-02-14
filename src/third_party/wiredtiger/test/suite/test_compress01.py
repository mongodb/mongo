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
# test_compress01.py
#    Smoke-test compression
#

import wttest
from wtscenario import make_scenarios

# Smoke-test compression
class test_compress01(wttest.WiredTigerTestCase):

    types = [
        ('file', dict(uri='file:test_compress01')),
        ('table', dict(uri='table:test_compress01')),
    ]
    compress = [
        ('nop', dict(compress='nop')),
        ('lz4', dict(compress='lz4')),
        ('lz4-noraw', dict(compress='lz4')),    # API compatibility test
        ('snappy', dict(compress='snappy')),
        ('zlib', dict(compress='zlib')),
        ('zlib-noraw', dict(compress='zlib')),  # API compatibility test
        ('zstd', dict(compress='zstd')),
    ]
    scenarios = make_scenarios(types, compress)

    nrecords = 10000
    bigvalue = "abcdefghij" * 1000

    # Load the compression extension, skip the test if missing
    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        extlist.extension('compressors', self.compress)

    # Create a table, add keys with both big and small values, then verify them.
    def test_compress(self):
        # Use relatively small leaf pages to force big values to be overflow
        # items, but still large enough that we get some compression action.
        params = 'key_format=S,value_format=S,leaf_page_max=4096'
        self.session.create(self.uri, params)
        cursor = self.session.open_cursor(self.uri, None)
        for idx in range(1,self.nrecords):
            cursor.set_key(repr(idx))
            if idx // 12 == 0:
                cursor.set_value(repr(idx) + self.bigvalue)
            else:
                cursor.set_value(repr(idx) + "abcdefg")
            cursor.insert()
        cursor.close()

        # Force the cache to disk, so we read compressed pages from disk.
        self.reopen_conn()

        cursor = self.session.open_cursor(self.uri, None)
        for idx in range(1,self.nrecords):
            cursor.set_key(repr(idx))
            self.assertEqual(cursor.search(), 0)
            if idx // 12 == 0:
                self.assertEqual(cursor.get_value(), repr(idx) + self.bigvalue)
            else:
                self.assertEqual(cursor.get_value(), repr(idx) + "abcdefg")
        cursor.close()

if __name__ == '__main__':
    wttest.run()
