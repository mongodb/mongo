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

import os, time
import wiredtiger, wttest
from helper import confirm_does_not_exist
from wtdataset import SimpleDataSet, ComplexDataSet
from wtscenario import make_scenarios

# test_drop.py
#    session level drop operation
class test_drop(wttest.WiredTigerTestCase):
    name = 'test_drop'
    extra_config = ''

    scenarios = make_scenarios([
        ('file', dict(uri='file:')),
        ('table', dict(uri='table:')),
        ('table-lsm', dict(uri='table:', extra_config=',type=lsm')),
    ])

    # Populate an object, remove it and confirm it no longer exists.
    def drop(self, dataset, with_cursor, reopen, drop_index):
        uri = self.uri + self.name
        ds = dataset(self, uri, 10, config=self.extra_config)
        ds.populate()

        # Open cursors should cause failure.
        if with_cursor:
            cursor = self.session.open_cursor(uri, None, None)
            self.assertRaises(wiredtiger.WiredTigerError,
                lambda: self.session.drop(uri, None))
            cursor.close()

        if reopen:
            self.reopen_conn()

        if drop_index:
            drop_uri = ds.index_name(0)
        else:
            drop_uri = uri
        self.dropUntilSuccess(self.session, drop_uri)
        confirm_does_not_exist(self, drop_uri)

    # Test drop of an object.
    def test_drop(self):
        # Simple file or table object.
        # Try all combinations except dropping the index, the simple
        # case has no indices.
        for with_cursor in [False, True]:
            for reopen in [False, True]:
                self.drop(SimpleDataSet, with_cursor, reopen, False)

        # A complex, multi-file table object.
        # Try all test combinations.
        if self.uri == "table:":
            for with_cursor in [False, True]:
                for reopen in [False, True]:
                    for drop_index in [False, True]:
                        self.drop(ComplexDataSet, with_cursor,
                                  reopen, drop_index)

    # Test drop of a non-existent object: force succeeds, without force fails.
    def test_drop_dne(self):
        uri = self.uri + self.name
        cguri = 'colgroup:' + self.name
        idxuri = 'index:' + self.name + ':indexname'
        lsmuri = 'lsm:' + self.name
        confirm_does_not_exist(self, uri)
        self.session.drop(uri, 'force')
        self.assertRaises(
            wiredtiger.WiredTigerError, lambda: self.session.drop(uri, None))
        self.session.drop(cguri, 'force')
        self.assertRaises(
            wiredtiger.WiredTigerError, lambda: self.session.drop(cguri, None))
        self.session.drop(idxuri, 'force')
        self.assertRaises(
            wiredtiger.WiredTigerError, lambda: self.session.drop(idxuri, None))
        self.session.drop(lsmuri, 'force')
        self.assertRaises(
            wiredtiger.WiredTigerError, lambda: self.session.drop(lsmuri, None))

if __name__ == '__main__':
    wttest.run()
