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

import os, wiredtiger, wttest
from wiredtiger import stat

# Per-object in_memory on a normal on-disk connection: no data file, block_size stays zero.
class test_inmem03(wttest.WiredTigerTestCase):
    uri = 'file:test_inmem03.wt'
    table_config = 'key_format=i,value_format=S,in_memory=true,log=(enabled=false)'
    disk_uri = 'file:test_inmem03_disk.wt'
    disk_config = 'key_format=i,value_format=S,log=(enabled=false)'

    def data_filename(self):
        return 'test_inmem03.wt'

    def disk_data_filename(self):
        return 'test_inmem03_disk.wt'

    def block_size(self, uri=None):
        if uri is None:
            uri = self.uri
        cstat = self.session.open_cursor('statistics:' + uri, None, 'statistics=(all)')
        sz = cstat[stat.dsrc.block_size][2]
        cstat.close()
        return sz

    def test_no_data_file_and_block_size(self):
        self.session.create(self.uri, self.table_config)
        self.assertFalse(os.path.exists(self.data_filename()))

        cursor = self.session.open_cursor(self.uri)
        cursor[1] = 'abc'
        cursor.close()
        self.session.checkpoint()
        self.assertFalse(os.path.exists(self.data_filename()))
        self.assertEqual(self.block_size(), 0)

        cursor = self.session.open_cursor(self.uri)
        self.assertEqual(cursor[1], 'abc')
        cursor.close()

    def test_on_disk_block_size_after_checkpoint(self):
        self.session.create(self.disk_uri, self.disk_config)
        cursor = self.session.open_cursor(self.disk_uri)
        cursor[1] = 'abc'
        cursor.close()
        self.session.checkpoint()
        self.assertTrue(os.path.exists(self.disk_data_filename()))
        self.assertGreater(self.block_size(self.disk_uri), 0)

    def test_reopen_no_data_file(self):
        self.session.create(self.uri, self.table_config)
        cursor = self.session.open_cursor(self.uri)
        cursor[42] = 'volatile'
        cursor.close()
        self.session.checkpoint()

        self.reopen_conn()
        self.assertFalse(os.path.exists(self.data_filename()))
        self.assertEqual(self.block_size(), 0)
        self.session.open_cursor(self.uri).close()

    # Operations that read or rewrite the data file fail cleanly when there is none.
    def test_no_data_file_operations(self):
        self.session.create(self.uri, self.table_config)
        cursor = self.session.open_cursor(self.uri)
        cursor[1] = 'abc'
        cursor.close()
        self.session.checkpoint()

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.salvage(self.uri, None), '/no backing file/')
        if wiredtiger.diagnostic_build():
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.session.verify(self.uri, 'dump_offsets=[0]'), '/no backing file/')
        self.session.open_cursor(self.uri).close()
