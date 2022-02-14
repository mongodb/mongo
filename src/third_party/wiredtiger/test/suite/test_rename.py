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
from helper import confirm_does_not_exist
from wtdataset import SimpleDataSet, ComplexDataSet
from wtscenario import make_scenarios

# test_rename.py
#    session level rename operation
class test_rename(wttest.WiredTigerTestCase):
    name1 = 'test_rename1'
    name2 = 'test_rename2'

    scenarios = make_scenarios([
        ('file', dict(uri='file:')),
        ('table', dict(uri='table:'))
    ])

    # Populate and object, and rename it a couple of times, confirming the
    # old name doesn't exist and the new name has the right contents.
    def rename(self, dataset, with_cursor):
        uri1 = self.uri + self.name1
        uri2 = self.uri + self.name2
        # Only populate uri1, we keep ds2 for checking.
        ds1 = dataset(self, uri1, 10)
        ds2 = dataset(self, uri2, 10)
        ds1.populate()

        # Open cursors should cause failure.
        if with_cursor:
            cursor = self.session.open_cursor(uri1, None, None)
            self.assertRaises(wiredtiger.WiredTigerError,
                lambda: self.session.rename(uri1, uri2, None))
            cursor.close()

        self.session.rename(uri1, uri2, None)
        confirm_does_not_exist(self, uri1)
        ds2.check()

        self.session.rename(uri2, uri1, None)
        confirm_does_not_exist(self, uri2)
        ds1.check()

        self.session.drop(uri1)

    # Test rename of an object.
    def test_rename(self):
        # Simple, one-file file or table object.
        self.rename(SimpleDataSet, False)
        self.rename(SimpleDataSet, True)

        # A complex, multi-file table object.
        if self.uri == "table:":
            self.rename(ComplexDataSet, False)
            self.rename(ComplexDataSet, True)

    def test_rename_dne(self):
        uri1 = self.uri + self.name1
        uri2 = self.uri + self.name2
        confirm_does_not_exist(self, uri1)
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.rename(uri1, uri2, None))

    def test_rename_bad_uri(self):
        uri1 = self.uri + self.name1
        if self.uri == "file:":
                uri2 = "table:" + self.name2
        else:
                uri2 = "file:" + self.name2
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.rename(uri1, uri2, None),
            '/type must match URI/')

if __name__ == '__main__':
    wttest.run()
