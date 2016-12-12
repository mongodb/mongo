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

import wiredtiger, wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_overwrite.py
#    cursor overwrite configuration method
class test_overwrite(wttest.WiredTigerTestCase):
    name = 'overwrite'
    scenarios = make_scenarios([
        ('file-r', dict(type='file:', keyfmt='r', dataset=SimpleDataSet)),
        ('file-S', dict(type='file:', keyfmt='S', dataset=SimpleDataSet)),
        ('lsm-S', dict(type='lsm:', keyfmt='S', dataset=SimpleDataSet)),
        ('table-r', dict(type='table:', keyfmt='r', dataset=SimpleDataSet)),
        ('table-S', dict(type='table:', keyfmt='S', dataset=SimpleDataSet)),
    ])

    # Confirm a cursor configured with/without overwrite correctly handles
    # non-existent records during insert, remove and update operations.
    def test_overwrite_insert(self):
        uri = self.type + self.name
        ds = self.dataset(self, uri, 100, key_format=self.keyfmt)
        ds.populate()

        # Insert of an existing record with overwrite off fails.
        cursor = self.session.open_cursor(uri, None, "overwrite=false")
        cursor.set_key(ds.key(5))
        cursor.set_value('XXXXXXXXXX')
        self.assertRaises(wiredtiger.WiredTigerError, lambda: cursor.insert())

        # One additional test for the insert method: duplicate the cursor with
        # overwrite configured and then the insert should succeed.  This test
        # is only for the insert method because the remove and update method
        # failure modes are for non-existent records, and you cannot duplicate
        # cursor pointing to non-existent records.
        cursor = self.session.open_cursor(uri, None, "overwrite=false")
        cursor.set_key(ds.key(5))
        dupc = self.session.open_cursor(None, cursor, "overwrite=true")
        dupc.set_value('XXXXXXXXXX')
        self.assertEquals(dupc.insert(), 0)

        # Insert of an existing record with overwrite on succeeds.
        cursor = self.session.open_cursor(uri, None)
        cursor.set_key(ds.key(6))
        cursor.set_value('XXXXXXXXXX')
        self.assertEquals(cursor.insert(), 0)

        # Insert of a non-existent record with overwrite off succeeds.
        cursor = self.session.open_cursor(uri, None, "overwrite=false")
        cursor.set_key(ds.key(200))
        cursor.set_value('XXXXXXXXXX')
        self.assertEquals(cursor.insert(), 0)

        # Insert of a non-existent record with overwrite on succeeds.
        cursor = self.session.open_cursor(uri, None)
        cursor.set_key(ds.key(201))
        cursor.set_value('XXXXXXXXXX')
        self.assertEquals(cursor.insert(), 0)

    def test_overwrite_remove(self):
        uri = self.type + self.name
        ds = self.dataset(self, uri, 100, key_format=self.keyfmt)
        ds.populate()

        # Remove of an existing record with overwrite off succeeds.
        cursor = self.session.open_cursor(uri, None, "overwrite=false")
        cursor.set_key(ds.key(5))
        self.assertEquals(cursor.remove(), 0)

        # Remove of an existing record with overwrite on succeeds.
        cursor = self.session.open_cursor(uri, None)
        cursor.set_key(ds.key(6))
        self.assertEquals(cursor.remove(), 0)

        # Remove of a non-existent record with overwrite off fails.
        cursor = self.session.open_cursor(uri, None, "overwrite=false")
        cursor.set_key(ds.key(200))
        self.assertEquals(cursor.remove(), wiredtiger.WT_NOTFOUND)

        # Remove of a non-existent record with overwrite on succeeds.
        cursor = self.session.open_cursor(uri, None)
        cursor.set_key(ds.key(201))
        self.assertEquals(cursor.remove(), 0)

    def test_overwrite_update(self):
        uri = self.type + self.name
        ds = self.dataset(self, uri, 100, key_format=self.keyfmt)
        ds.populate()

        # Update of an existing record with overwrite off succeeds.
        cursor = self.session.open_cursor(uri, None, "overwrite=false")
        cursor.set_key(ds.key(5))
        cursor.set_value('XXXXXXXXXX')
        self.assertEquals(cursor.update(), 0)

        # Update of an existing record with overwrite on succeeds.
        cursor = self.session.open_cursor(uri, None)
        cursor.set_key(ds.key(6))
        cursor.set_value('XXXXXXXXXX')
        self.assertEquals(cursor.update(), 0)

        # Update of a non-existent record with overwrite off fails.
        cursor = self.session.open_cursor(uri, None, "overwrite=false")
        cursor.set_key(ds.key(200))
        cursor.set_value('XXXXXXXXXX')
        self.assertEquals(cursor.update(), wiredtiger.WT_NOTFOUND)

        # Update of a non-existent record with overwrite on succeeds.
        cursor = self.session.open_cursor(uri, None)
        cursor.set_key(ds.key(201))
        cursor.set_value('XXXXXXXXXX')
        self.assertEquals(cursor.update(), 0)

if __name__ == '__main__':
    wttest.run()
