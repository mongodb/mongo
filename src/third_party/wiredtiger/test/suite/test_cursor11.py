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
from wtdataset import SimpleLSMDataSet, ComplexDataSet, ComplexLSMDataSet
from wtscenario import make_scenarios

# test_cursor11.py
#    WT_CURSOR position tests: remove (if not already positioned), and insert
#    leave the cursor without position or information.
class test_cursor11(wttest.WiredTigerTestCase):

    keyfmt = [
        ('integer', dict(keyfmt='i', valfmt='S')),
        ('recno', dict(keyfmt='r', valfmt='S')),
        ('recno-fix', dict(keyfmt='r', valfmt='8t')),
        ('string', dict(keyfmt='S', valfmt='S')),
    ]
    types = [
        ('file', dict(uri='file', ds=SimpleDataSet)),
        ('lsm', dict(uri='lsm', ds=SimpleDataSet)),
        ('table-complex', dict(uri='table', ds=ComplexDataSet)),
        ('table-complex-lsm', dict(uri='table', ds=ComplexLSMDataSet)),
        ('table-index', dict(uri='table', ds=SimpleIndexDataSet)),
        ('table-simple', dict(uri='table', ds=SimpleDataSet)),
        ('table-simple-lsm', dict(uri='table', ds=SimpleLSMDataSet)),
    ]

    # Discard invalid or unhelpful scenario combinations.
    def keep(name, d):
        if d['keyfmt'] == 'r' and (d['ds'].is_lsm() or d['uri'] == 'lsm'):
            return False
        if d['valfmt'] == '8t' and d['keyfmt'] != 'r':
            return False
        if d['valfmt'] == '8t' and d['ds'] == ComplexDataSet:
            return False
        return True

    scenarios = make_scenarios(types, keyfmt, include=keep)

    # Do a remove using the cursor after setting a position, and confirm
    # the key and position remain set but no value.
    def test_cursor_remove_with_position(self):
        # Build an object.
        uri = self.uri + ':test_cursor11'
        ds = self.ds(self, uri, 50, key_format=self.keyfmt)
        ds.populate()
        s = self.conn.open_session()
        c = ds.open_cursor()

        c.set_key(ds.key(25))
        self.assertEquals(c.search(), 0)
        self.assertEquals(c.next(), 0)
        self.assertEquals(c.get_key(), ds.key(26))
        c.remove()
        self.assertEquals(c.get_key(), ds.key(26))
        msg = '/requires value be set/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, c.get_value, msg)
        self.assertEquals(c.next(), 0)
        self.assertEquals(c.get_key(), ds.key(27))

    # Do a remove using the cursor without setting a position, and confirm
    # no key, value or position remains.
    def test_cursor_remove_without_position(self):
        # Build an object.
        uri = self.uri + ':test_cursor11'
        ds = self.ds(self, uri, 50, key_format=self.keyfmt)
        ds.populate()
        s = self.conn.open_session()
        c = ds.open_cursor()

        c.set_key(ds.key(25))
        c.remove()
        msg = '/requires key be set/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, c.get_key, msg)
        msg = '/requires value be set/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, c.get_value, msg)
        self.assertEquals(c.next(), 0)
        self.assertEquals(c.get_key(), ds.key(1))

    # Do a remove using the key after also setting a position, and confirm
    # no key, value or position remains.
    def test_cursor_remove_with_key_and_position(self):
        # Build an object.
        uri = self.uri + ':test_cursor11'
        ds = self.ds(self, uri, 50, key_format=self.keyfmt)
        ds.populate()
        s = self.conn.open_session()
        c = ds.open_cursor()

        c.set_key(ds.key(25))
        self.assertEquals(c.search(), 0)
        c.set_key(ds.key(25))
        c.remove()
        msg = '/requires key be set/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, c.get_key, msg)
        msg = '/requires value be set/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, c.get_value, msg)
        self.assertEquals(c.next(), 0)
        self.assertEquals(c.get_key(), ds.key(1))

    # Do an insert and confirm no key, value or position remains.
    def test_cursor_insert(self):
        # Build an object.
        uri = self.uri + ':test_cursor11'
        ds = self.ds(self, uri, 50, key_format=self.keyfmt)
        ds.populate()
        s = self.conn.open_session()
        c = ds.open_cursor()

        c.set_key(ds.key(25))
        c.set_value(ds.value(300))
        c.insert()
        msg = '/requires key be set/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, c.get_key, msg)
        msg = '/requires value be set/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, c.get_value, msg)
        self.assertEquals(c.next(), 0)
        self.assertEquals(c.get_key(), ds.key(1))

if __name__ == '__main__':
    wttest.run()
