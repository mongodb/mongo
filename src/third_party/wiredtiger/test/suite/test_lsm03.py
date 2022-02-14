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
# THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

import wiredtiger, wtscenario, wttest
from wtdataset import SimpleDataSet

# test_lsm03.py
#    Check to make sure that LSM schema operations don't get EBUSY when
#    there are no user operations active.
class test_lsm03(wttest.WiredTigerTestCase):
    name = 'test_lsm03'

    # Use small pages so we generate some internal layout
    # Setup LSM so multiple chunks are present
    config = 'allocation_size=512,internal_page_max=512' + \
             ',leaf_page_max=1k,lsm=(chunk_size=512k,merge_min=10)'

    # Populate an object then drop it.
    def test_lsm_drop_active(self):
        uri = 'lsm:' + self.name
        ds = SimpleDataSet(self, uri, 10000, config=self.config)
        ds.populate()

        # Force to disk
        self.reopen_conn()

        # An open cursors should cause failure.
        cursor = self.session.open_cursor(uri, None, None)
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.drop(uri, None))
        cursor.close()

        # Add enough records that a merge should be running
        ds = SimpleDataSet(self, uri, 50000, config=self.config)
        ds.populate()
        # The drop should succeed even when LSM work units are active
        self.dropUntilSuccess(self.session, uri)
