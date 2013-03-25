#!/usr/bin/env python
#
# Public Domain 2008-2013 WiredTiger, Inc.
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
        ('file', dict(type='file:')),
        ('table', dict(type='table:'))
    ]
    keyfmt = [
        ('integer', dict(keyfmt='i')),
        ('recno', dict(keyfmt='r')),
        ('string', dict(keyfmt='S'))
    ]
    scenarios = number_scenarios(multiply_scenarios('.', types, keyfmt))

    def test_cursor_comparison(self):
        uri = self.type + 'compare'

        # Build the object.
        if self.type == 'file:':
            simple_populate(self, uri, 'key_format=' + self.keyfmt, 100)
        else:
            complex_populate(self, uri, 'key_format=' + self.keyfmt, 100)

        c1 = self.session.open_cursor(uri, None)
        c2 = self.session.open_cursor(uri, None)

        # Confirm the method fails unless the keys are set.
        msg = '/requires key be set/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: c1.compare(c2), msg)
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: c2.compare(c1), msg)

        # Test cursors in all three orders.
        c1.set_key(key_populate(c1, 10))
        self.assertEquals(c1.search(), 0)
        c2.set_key(key_populate(c2, 20))
        self.assertEquals(c2.search(), 0)
        self.assertGreater(c2.compare(c1), 0)
        self.assertLess(c1.compare(c2), 0)
        c2.set_key(key_populate(c2, 10))
        self.assertEquals(c1.compare(c2), 0)


if __name__ == '__main__':
    wttest.run()
