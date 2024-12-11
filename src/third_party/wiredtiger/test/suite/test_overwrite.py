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
from wtdataset import SimpleDataSet, SimpleIndexDataSet
from wtdataset import ComplexDataSet
from wtscenario import filter_scenarios, make_scenarios

# test_overwrite.py
#    cursor overwrite configuration method
class test_overwrite(wttest.WiredTigerTestCase):
    name = 'overwrite'
    keyfmt = [
        ('fix', dict(keyfmt='r',valuefmt='8t')),
        ('row', dict(keyfmt='S',valuefmt='S')),
        ('row-int', dict(keyfmt='i',valuefmt='S')),
        ('var', dict(keyfmt='r',valuefmt='S')),
    ]
    types = [
        ('file', dict(uri='file:', ds=SimpleDataSet)),
        ('table-complex', dict(uri='table:', ds=ComplexDataSet)),
        ('table-index', dict(uri='table:', ds=SimpleIndexDataSet)),
        ('table-simple', dict(uri='table:', ds=SimpleDataSet)),
    ]

    # The cursor fast path logic checks against the exact string only, but other valid
    # configurations should also disable overwrite through the normal config parsing flow.
    cursor_configs = [
        ('cfg1', dict(cursor_cfg='overwrite=false', valid=True)),
        ('cfg2', dict(cursor_cfg='overwrite=false,', valid=True)),
        ('cfg3', dict(cursor_cfg='overwrite=false,,,', valid=True)),
        ('cfg4', dict(cursor_cfg=',,,,overwrite=false', valid=True)),
        ('cfg5', dict(cursor_cfg='append=false,overwrite=false', valid=True)),
    ]
    scenarios = make_scenarios(types, keyfmt, cursor_configs)

    # Confirm a cursor configured with/without overwrite correctly handles
    # non-existent records during insert and update operations.
    def test_overwrite_insert(self):
        uri = self.uri + self.name
        ds = self.ds(self, uri, 100, key_format=self.keyfmt, value_format=self.valuefmt)
        ds.populate()

        # Insert of an existing record with overwrite off fails.
        cursor = ds.open_cursor(uri, None, self.cursor_cfg)
        cursor.set_key(ds.key(5))
        cursor.set_value(ds.value(1000))
        self.assertRaises(wiredtiger.WiredTigerError, lambda: cursor.insert())

        # One additional test for the insert method: duplicate the cursor with overwrite
        # configured and then the insert should succeed.  This test is only for the insert method
        # because the update method's failure modes are for non-existent records, and you cannot
        # duplicate a cursor pointing to non-existent records.
        cursor = ds.open_cursor(uri, None, self.cursor_cfg)
        cursor.set_key(ds.key(5))
        dupc = self.session.open_cursor(None, cursor, "overwrite=true")
        dupc.set_value(ds.value(1001))
        self.assertEqual(dupc.insert(), 0)

        # Insert of an existing record with overwrite on succeeds.
        cursor = ds.open_cursor(uri, None)
        cursor.set_key(ds.key(6))
        cursor.set_value(ds.value(1002))
        self.assertEqual(cursor.insert(), 0)

        # Insert of a non-existent record with overwrite off succeeds.
        cursor = ds.open_cursor(uri, None, self.cursor_cfg)
        cursor.set_key(ds.key(200))
        cursor.set_value(ds.value(1003))
        self.assertEqual(cursor.insert(), 0)

        # Insert of a non-existent record with overwrite on succeeds.
        cursor = ds.open_cursor(uri, None)
        cursor.set_key(ds.key(201))
        cursor.set_value(ds.value(1004))
        self.assertEqual(cursor.insert(), 0)

    # Historically, overwrite applied to cursor.remove as well. Confirm that is no longer the case.
    def test_overwrite_remove(self):
        uri = self.uri + self.name
        ds = self.ds(self, uri, 100, key_format=self.keyfmt, value_format=self.valuefmt)
        ds.populate()

        # Remove of an existing record with overwrite off succeeds.
        cursor = ds.open_cursor(uri, None, self.cursor_cfg)
        cursor.set_key(ds.key(5))
        self.assertEqual(cursor.remove(), 0)

        # Remove of an existing record with overwrite on succeeds.
        cursor = ds.open_cursor(uri, None)
        cursor.set_key(ds.key(6))
        self.assertEqual(cursor.remove(), 0)

        # Remove of a non-existent record with overwrite off fails.
        cursor = ds.open_cursor(uri, None, self.cursor_cfg)
        cursor.set_key(ds.key(200))
        self.assertEqual(cursor.remove(), wiredtiger.WT_NOTFOUND)

        # Remove of a non-existent record with overwrite on fails.
        cursor = ds.open_cursor(uri, None)
        cursor.set_key(ds.key(201))
        self.assertEqual(cursor.remove(), wiredtiger.WT_NOTFOUND)

    def test_overwrite_update(self):
        uri = self.uri + self.name
        ds = self.ds(self, uri, 100, key_format=self.keyfmt, value_format=self.valuefmt)
        ds.populate()

        # Update of an existing record with overwrite off succeeds.
        cursor = ds.open_cursor(uri, None, self.cursor_cfg)
        cursor.set_key(ds.key(5))
        cursor.set_value(ds.value(1005))
        self.assertEqual(cursor.update(), 0)

        # Update of an existing record with overwrite on succeeds.
        cursor = ds.open_cursor(uri, None)
        cursor.set_key(ds.key(6))
        cursor.set_value(ds.value(1006))
        self.assertEqual(cursor.update(), 0)

        # Update of a non-existent record with overwrite off fails.
        cursor = ds.open_cursor(uri, None, self.cursor_cfg)
        cursor.set_key(ds.key(200))
        cursor.set_value(ds.value(1007))
        self.assertEqual(cursor.update(), wiredtiger.WT_NOTFOUND)

        # Update of a non-existent record with overwrite on succeeds.
        cursor = ds.open_cursor(uri, None)
        cursor.set_key(ds.key(201))
        cursor.set_value(ds.value(1008))
        self.assertEqual(cursor.update(), 0)
