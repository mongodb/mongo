#!/usr/bin/env python
#
# Public Domain 2014-2015 MongoDB, Inc.
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
from helper import complex_populate, simple_populate, key_populate
from wtscenario import multiply_scenarios, number_scenarios

# Test cursor comparisons.
class test_cursor_comparison(wttest.WiredTigerTestCase):
    name = 'test_compare'

    types = [
        ('file', dict(type='file:', config='')),
        ('lsm', dict(type='table:', config=',type=lsm')),
        ('table', dict(type='table:', config=''))
    ]
    keyfmt = [
        ('integer', dict(keyfmt='i')),
        ('recno', dict(keyfmt='r')),
        ('string', dict(keyfmt='S'))
    ]
    scenarios = number_scenarios(multiply_scenarios('.', types, keyfmt))

    def test_cursor_comparison(self):
        uri = self.type + 'compare'
        uriX = self.type + 'compareX'

        # Build the object.
        if self.type == 'file:':
            simple_populate(self, uri,
                            'key_format=' + self.keyfmt + self.config, 100)
            simple_populate(self, uriX,
                            'key_format=' + self.keyfmt + self.config, 100)
        else:
            complex_populate(self, uri,
                             'key_format=' + self.keyfmt + self.config, 100)
            complex_populate(self, uriX,
                             'key_format=' + self.keyfmt + self.config, 100)

        c1 = self.session.open_cursor(uri, None)
        c2 = self.session.open_cursor(uri, None)

        # Confirm failure unless the keys are set.
        msg = '/requires key be set/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: c1.compare(c2), msg)
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: c2.compare(c1), msg)

        # Test cursors before they're positioned.
        c1.set_key(key_populate(c1, 10))
        c2.set_key(key_populate(c2, 20))
        self.assertGreater(c2.compare(c1), 0)
        self.assertLess(c1.compare(c2), 0)
        c2.set_key(key_populate(c2, 10))
        self.assertEqual(c1.compare(c2), 0)
        self.assertEqual(c2.compare(c1), 0)

        # Confirm failure for different objects.
        cX = self.session.open_cursor(uriX, None)
        cX.set_key(key_populate(cX, 10))
        msg = '/must reference the same object/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: cX.compare(c1), msg)

        # Test cursors after they're positioned (shouldn't matter for compare).
        c1.set_key(key_populate(c1, 10))
        self.assertEqual(c1.search(), 0)
        c2.set_key(key_populate(c2, 20))
        self.assertEqual(c2.search(), 0)
        self.assertGreater(c2.compare(c1), 0)
        self.assertLess(c1.compare(c2), 0)
        c2.set_key(key_populate(c2, 10))
        self.assertEqual(c2.search(), 0)
        self.assertEqual(c1.compare(c2), 0)
        self.assertEqual(c2.compare(c1), 0)

        # Confirm failure for different objects.
        cX = self.session.open_cursor(uriX, None)
        cX.set_key(key_populate(cX, 10))
        self.assertEqual(cX.search(), 0)
        msg = '/must reference the same object/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: cX.compare(c1), msg)

    def test_cursor_equality(self):
        uri = self.type + 'equality'
        uriX = self.type + 'compareX'

        # Build the object.
        if self.type == 'file:':
            simple_populate(self, uri,
                            'key_format=' + self.keyfmt + self.config, 100)
            simple_populate(self, uriX,
                            'key_format=' + self.keyfmt + self.config, 100)
        else:
            complex_populate(self, uri,
                             'key_format=' + self.keyfmt + self.config, 100)
            complex_populate(self, uriX,
                             'key_format=' + self.keyfmt + self.config, 100)

        c1 = self.session.open_cursor(uri, None)
        c2 = self.session.open_cursor(uri, None)

        # Confirm failure unless the keys are set.
        msg = '/requires key be set/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: c1.equals(c2), msg)
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: c2.equals(c1), msg)

        # Test cursors before they're positioned.
        c1.set_key(key_populate(c1, 10))
        c2.set_key(key_populate(c2, 20))
        self.assertFalse(c1.equals(c2))
        self.assertFalse(c2.equals(c1))
        c2.set_key(key_populate(c2, 10))
        self.assertTrue(c1.equals(c2))
        self.assertTrue(c2.equals(c1))

        # Confirm failure for different objects.
        cX = self.session.open_cursor(uriX, None)
        cX.set_key(key_populate(cX, 10))
        msg = '/must reference the same object/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: cX.compare(c1), msg)

        # Test cursors after they're positioned (internally, it's a different
        # search path if keys are positioned in the tree).
        c1.set_key(key_populate(c1, 10))
        self.assertEqual(c1.search(), 0)
        c2.set_key(key_populate(c2, 20))
        self.assertEqual(c2.search(), 0)
        self.assertFalse(c1.equals(c2))
        self.assertFalse(c2.equals(c1))
        c2.set_key(key_populate(c2, 10))
        self.assertEqual(c2.search(), 0)
        self.assertTrue(c1.equals(c2))
        self.assertTrue(c2.equals(c1))

        # Confirm failure for different objects.
        cX = self.session.open_cursor(uriX, None)
        cX.set_key(key_populate(cX, 10))
        self.assertEqual(cX.search(), 0)
        msg = '/must reference the same object/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: cX.compare(c1), msg)


if __name__ == '__main__':
    wttest.run()
