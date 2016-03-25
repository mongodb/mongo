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

import os, time
import wiredtiger, wttest
from helper import complex_populate, simple_populate
from wtscenario import check_scenarios

# test_rebalance.py
#    session level rebalance operation
class test_rebalance(wttest.WiredTigerTestCase):
    name = 'test_rebalance'

    # Use small pages so we generate some internal layout
    # Setup LSM so multiple chunks are present
    config = 'key_format=S,allocation_size=512,internal_page_max=512' + \
             ',leaf_page_max=1k,lsm=(chunk_size=512k,merge_min=10)'

    scenarios = check_scenarios([
        ('file', dict(uri='file:')),
        ('table', dict(uri='table:')),
        ('lsm', dict(uri='lsm:'))
    ])

    # Populate an object, then rebalance it.
    def rebalance(self, populate, with_cursor):
        uri = self.uri + self.name
        populate(self, uri, self.config, 10000)

        # Force to disk, we don't rebalance in-memory objects.
        self.reopen_conn()

        # Open cursors should cause failure.
        if with_cursor:
            cursor = self.session.open_cursor(uri, None, None)
            self.assertRaises(wiredtiger.WiredTigerError,
                lambda: self.session.rebalance(uri, None))
            cursor.close()

        self.session.rebalance(uri, None)
        self.session.drop(uri)

    # Test rebalance of an object.
    def test_rebalance(self):
        # Simple file or table object.
        self.rebalance(simple_populate, False)
        self.rebalance(simple_populate, True)

        # A complex, multi-file table object.
        if self.uri == "table:":
            self.rebalance(complex_populate, False)
            self.rebalance(complex_populate, True)


if __name__ == '__main__':
    wttest.run()
