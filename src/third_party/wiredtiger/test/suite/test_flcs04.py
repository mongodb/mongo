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

import wiredtiger, wttest
from wtdataset import SimpleDataSet

# test_flcs04.py
#
# Make sure modify fails cleanly on FLCS tables.

class test_flcs04(wttest.WiredTigerTestCase):
    session_config = 'isolation=snapshot'
    conn_config = 'in_memory=false'

    def test_flcs(self):
        uri = "table:test_flcs04"
        nrows = 10
        ds = SimpleDataSet(
            self, uri, nrows, key_format='r', value_format='6t', config='leaf_page_max=4096')
        ds.populate()

        
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()

        cursor.set_key(5)
        mods = [wiredtiger.Modify('Q', 100, 1)]
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: cursor.modify(mods),
            "/WT_CURSOR.modify only supported for/")

        self.session.rollback_transaction()
        cursor.close()
