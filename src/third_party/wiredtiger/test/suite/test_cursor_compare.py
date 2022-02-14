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
from wtdataset import SimpleDataSet, ComplexDataSet, ComplexLSMDataSet
from wtscenario import make_scenarios

# Test cursor comparisons.
class test_cursor_comparison(wttest.WiredTigerTestCase):
    name = 'test_compare'

    types = [
        ('file', dict(type='file:', lsm=False, dataset=SimpleDataSet)),
        ('lsm', dict(type='table:', lsm=True, dataset=ComplexLSMDataSet)),
        ('table', dict(type='table:', lsm=False, dataset=ComplexDataSet))
    ]
    keyfmt = [
        ('integer', dict(keyfmt='i', valfmt='S')),
        ('recno', dict(keyfmt='r', valfmt='S')),
        ('recno-fix', dict(keyfmt='r', valfmt='8t')),
        ('string', dict(keyfmt='S', valfmt='S'))
    ]

    # Discard invalid or unhelpful scenario combinations.
    def keep(name, d):
        if d['keyfmt'] == 'r':
            # Skip record number keys with LSM.
            if d['lsm']:
                return False
            # Skip complex data sets with FLCS.
            if d['valfmt'] == '8t' and d['dataset'] != SimpleDataSet:
                return False
        else:
            # Skip byte data with row-store.
            if d['valfmt'] == '8t':
                return False
        return True

    scenarios = make_scenarios(types, keyfmt, include=keep)

    def test_cursor_comparison(self):
        uri = self.type + 'compare'
        uriX = self.type + 'compareX'

        # Build the object.
        ds = self.dataset(self, uri, 100, key_format=self.keyfmt)
        dsX = self.dataset(self, uriX, 100, key_format=self.keyfmt)
        ds.populate()
        dsX.populate()
        if self.type == 'file:':
            ix0_0 = None
            ix0_1 = None
            ix1_0 = None
            ixX_0 = None
        else:
            ix0_0 = self.session.open_cursor(ds.index_name(0), None)
            ix0_1 = self.session.open_cursor(ds.index_name(0), None)
            ix1_0 = self.session.open_cursor(ds.index_name(1), None)
            ixX_0 = self.session.open_cursor(dsX.index_name(0), None)
            ix0_0.next()
            ix0_1.next()
            ix1_0.next()
            ixX_0.next()

        c1 = self.session.open_cursor(uri, None)
        c2 = self.session.open_cursor(uri, None)

        # Confirm failure unless the keys are set.
        msg = '/requires key be set/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: c1.compare(c2), msg)
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: c2.compare(c1), msg)

        # Test cursors before they're positioned.
        c1.set_key(ds.key(10))
        c2.set_key(ds.key(20))
        self.assertGreater(c2.compare(c1), 0)
        self.assertLess(c1.compare(c2), 0)
        c2.set_key(ds.key(10))
        self.assertEqual(c1.compare(c2), 0)
        self.assertEqual(c2.compare(c1), 0)

        # Confirm failure for different objects.
        cX = self.session.open_cursor(uriX, None)
        cX.set_key(dsX.key(10))
        msg = '/must reference the same object/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: cX.compare(c1), msg)
        msg = '/wt_cursor.* is None/'
        self.assertRaisesHavingMessage(
            RuntimeError,  lambda: cX.compare(None), msg)
        if ix0_0 != None:
            self.assertEqual(ix0_0.compare(ix0_1), 0)
            ix0_1.reset()
            ix0_1.prev()
            self.assertLess(ix0_0.compare(ix0_1), 0)
            self.assertGreater(ix0_1.compare(ix0_0), 0)
            # Main table vs. index not allowed
            msg = '/must reference the same object/'
            self.assertRaisesWithMessage(
                wiredtiger.WiredTigerError, lambda: c1.compare(ix0_0), msg)
            # Two unrelated indices not allowed
            self.assertRaisesWithMessage(
                wiredtiger.WiredTigerError, lambda: ixX_0.compare(ix0_0), msg)
            # Two different indices from same table not allowed
            self.assertRaisesWithMessage(
                wiredtiger.WiredTigerError, lambda: ix0_0.compare(ix1_0), msg)

        # Test cursors after they're positioned (shouldn't matter for compare).
        c1.set_key(ds.key(10))
        self.assertEqual(c1.search(), 0)
        c2.set_key(ds.key(20))
        self.assertEqual(c2.search(), 0)
        self.assertGreater(c2.compare(c1), 0)
        self.assertLess(c1.compare(c2), 0)
        c2.set_key(ds.key(10))
        self.assertEqual(c2.search(), 0)
        self.assertEqual(c1.compare(c2), 0)
        self.assertEqual(c2.compare(c1), 0)

        # Confirm failure for different objects.
        cX = self.session.open_cursor(uriX, None)
        cX.set_key(dsX.key(10))
        self.assertEqual(cX.search(), 0)
        msg = '/must reference the same object/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: cX.compare(c1), msg)

    def test_cursor_equality(self):
        uri = self.type + 'equality'
        uriX = self.type + 'compareX'

        # Build the object.
        ds = self.dataset(self, uri, 100, key_format=self.keyfmt)
        dsX = self.dataset(self, uriX, 100, key_format=self.keyfmt)
        ds.populate()
        dsX.populate()
        if self.type == 'file:':
            ix0_0 = None
            ix0_1 = None
            ix1_0 = None
            ixX_0 = None
        else:
            ix0_0 = self.session.open_cursor(ds.index_name(0), None)
            ix0_1 = self.session.open_cursor(ds.index_name(0), None)
            ix1_0 = self.session.open_cursor(ds.index_name(1), None)
            ixX_0 = self.session.open_cursor(dsX.index_name(0), None)
            ix0_0.next()
            ix0_1.next()
            ix1_0.next()
            ixX_0.next()

        c1 = self.session.open_cursor(uri, None)
        c2 = self.session.open_cursor(uri, None)

        # Confirm failure unless the keys are set.
        msg = '/requires key be set/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: c1.equals(c2), msg)
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: c2.equals(c1), msg)

        # Test cursors before they're positioned.
        c1.set_key(ds.key(10))
        c2.set_key(ds.key(20))
        self.assertFalse(c1.equals(c2))
        self.assertFalse(c2.equals(c1))
        c2.set_key(ds.key(10))
        self.assertTrue(c1.equals(c2))
        self.assertTrue(c2.equals(c1))

        # Confirm failure for different objects.
        cX = self.session.open_cursor(uriX, None)
        cX.set_key(dsX.key(10))
        msg = '/must reference the same object/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: cX.equals(c1), msg)
        msg = '/wt_cursor.* is None/'
        self.assertRaisesHavingMessage(
            RuntimeError,  lambda: cX.equals(None), msg)
        if ix0_0 != None:
            self.assertTrue(ix0_0.equals(ix0_1))
            ix0_1.reset()
            ix0_1.prev()
            self.assertFalse(ix0_0.equals(ix0_1))
            # Main table vs. index not allowed
            msg = '/must reference the same object/'
            self.assertRaisesWithMessage(
                wiredtiger.WiredTigerError, lambda: c1.equals(ix0_0), msg)
            # Two unrelated indices not allowed
            self.assertRaisesWithMessage(
                wiredtiger.WiredTigerError, lambda: ixX_0.equals(ix0_0), msg)
            # Two different indices from same table not allowed
            self.assertRaisesWithMessage(
                wiredtiger.WiredTigerError, lambda: ix0_0.equals(ix1_0), msg)

        # Test cursors after they're positioned (internally, it's a different
        # search path if keys are positioned in the tree).
        c1.set_key(ds.key(10))
        self.assertEqual(c1.search(), 0)
        c2.set_key(ds.key(20))
        self.assertEqual(c2.search(), 0)
        self.assertFalse(c1.equals(c2))
        self.assertFalse(c2.equals(c1))
        c2.set_key(ds.key(10))
        self.assertEqual(c2.search(), 0)
        self.assertTrue(c1.equals(c2))
        self.assertTrue(c2.equals(c1))

        # Confirm failure for different objects.
        cX = self.session.open_cursor(uriX, None)
        cX.set_key(dsX.key(10))
        self.assertEqual(cX.search(), 0)
        msg = '/must reference the same object/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: cX.equals(c1), msg)

if __name__ == '__main__':
    wttest.run()
