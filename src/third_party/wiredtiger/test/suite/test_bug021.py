#!/usr/bin/env python
#
# Public Domain 2014-2018 MongoDB, Inc.
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
#
# test_bug021.py
#       Fixed-length column store implicit record operations test.

import wiredtiger, wttest

# Fixed-length column store implicit record operations test.
class test_bug021(wttest.WiredTigerTestCase):
    uri = 'file:test_bug021'

    def create_implicit(self, initial, middle, trailing):
        self.session.create(self.uri, 'key_format=r,value_format=8t')
        cursor = self.session.open_cursor(self.uri, None)

        # Create a set of initial implicit records, followed by a set of real
        # records, followed by a set of trailing implicit records.
        expected = [0] * (initial + middle + trailing + 2)
        expected[0] = None
        r = 0
        for i in range(initial):
            r += 1
            expected[r] = 0x00
        for i in range(middle):
            r += 1
            cursor[r] = expected[r] = 0xab
        r += trailing
        cursor[r + 1] = expected[r + 1] = 0xab
        return (cursor, expected)

    def check(self, expected):
        c = self.session.open_cursor(self.uri, None)
        actual = [None] * len(expected)
        for k, v in c:
                actual[k] = v
        c.close()

        if actual != expected:
            print 'expected: ', expected
            print '  actual: ', actual
        self.assertEqual(expected, actual)

    def test_implicit_record_cursor_insert_next(self):
        cursor, current = self.create_implicit(0, 50, 20)
        self.check(current)

        # Check cursor next/operation inside trailing implicit keys.
        cursor.set_key(62)
        self.assertEquals(cursor.search(), 0)
        self.assertEquals(cursor.next(), 0)
        self.assertEquals(cursor.next(), 0)
        cursor.set_value(3)
        self.assertEquals(cursor.insert(), 0)
        current[62 + 2] = 3
        self.check(current)

        # Check cursor prev/operation inside trailing implicit keys.
        cursor.set_key(68)
        self.assertEquals(cursor.search(), 0)
        self.assertEquals(cursor.prev(), 0)
        self.assertEquals(cursor.prev(), 0)
        cursor.set_value(7)
        self.assertEquals(cursor.insert(), 0)
        current[68 - 2] = 7

    def test_implicit_record_cursor_insert_prev(self):
        cursor, current = self.create_implicit(20, 50, 0)
        self.check(current)

        # Check cursor next/operation inside leading implicit keys.
        cursor.set_key(2)
        self.assertEquals(cursor.search(), 0)
        self.assertEquals(cursor.next(), 0)
        self.assertEquals(cursor.next(), 0)
        cursor.set_value(3)
        self.assertEquals(cursor.insert(), 0)
        current[2 + 2] = 3
        self.check(current)

        # Check cursor prev/operation inside leading implicit keys.
        cursor.set_key(18)
        self.assertEquals(cursor.search(), 0)
        self.assertEquals(cursor.prev(), 0)
        self.assertEquals(cursor.prev(), 0)
        cursor.set_value(7)
        self.assertEquals(cursor.insert(), 0)
        current[18 - 2] = 7
        self.check(current)

    def test_implicit_record_cursor_remove_next(self):
        cursor, current = self.create_implicit(0, 50, 20)
        self.check(current)

        # Check cursor next/operation inside trailing implicit keys.
        cursor.set_key(62)
        self.assertEquals(cursor.search(), 0)
        for i in range(1, 5):
            self.assertEquals(cursor.next(), 0)
            self.assertEquals(cursor.remove(), 0)
            current[62 + i] = 0
        self.check(current)

        # Check cursor prev/operation inside trailing implicit keys.
        cursor.set_key(68)
        self.assertEquals(cursor.search(), 0)
        for i in range(1, 5):
            self.assertEquals(cursor.prev(), 0)
            self.assertEquals(cursor.remove(), 0)
            current[68 - i] = 0
        self.check(current)

    def test_implicit_record_cursor_remove_prev(self):
        cursor, current = self.create_implicit(20, 50, 0)
        self.check(current)

        # Check cursor next/operation inside leading implicit keys.
        cursor.set_key(2)
        self.assertEquals(cursor.search(), 0)
        for i in range(1, 5):
            self.assertEquals(cursor.next(), 0)
            self.assertEquals(cursor.remove(), 0)
            current[2 + i] = 0
        self.check(current)

        # Check cursor prev/operation inside leading implicit keys.
        cursor.set_key(18)
        self.assertEquals(cursor.search(), 0)
        for i in range(1, 5):
            current[18 - i] = 0
            self.assertEquals(cursor.prev(), 0)
            self.assertEquals(cursor.remove(), 0)
            current[18 - i] = 0
        self.check(current)

    def test_implicit_record_cursor_update_next(self):
        cursor, current = self.create_implicit(0, 50, 20)
        self.check(current)

        # Check cursor next/operation inside trailing implicit keys.
        cursor.set_key(62)
        self.assertEquals(cursor.search(), 0)
        for i in range(1, 5):
            self.assertEquals(cursor.next(), 0)
            cursor.set_value(i)
            self.session.breakpoint()
            self.assertEquals(cursor.update(), 0)
            current[62 + i] = i
        self.check(current)

        # Check cursor prev/operation inside trailing implicit keys.
        cursor.set_key(68)
        self.assertEquals(cursor.search(), 0)
        for i in range(1, 5):
            self.assertEquals(cursor.prev(), 0)
            cursor.set_value(i)
            self.assertEquals(cursor.update(), 0)
            current[68 - i] = i
        self.check(current)

    def test_implicit_record_cursor_update_prev(self):
        cursor, current = self.create_implicit(20, 50, 0)
        self.check(current)

        # Check cursor next/operation inside leading implicit keys.
        cursor.set_key(2)
        self.assertEquals(cursor.search(), 0)
        for i in range(1, 5):
            self.assertEquals(cursor.next(), 0)
            cursor.set_value(i)
            self.assertEquals(cursor.update(), 0)
            current[2 + i] = i
        self.check(current)

        # Check cursor prev/operation inside leading implicit keys.
        cursor.set_key(18)
        self.assertEquals(cursor.search(), 0)
        for i in range(1, 5):
            current[18 - i] = 0
            self.assertEquals(cursor.prev(), 0)
            cursor.set_value(i)
            self.assertEquals(cursor.update(), 0)
            current[18 - i] = i
        self.check(current)

if __name__ == '__main__':
    wttest.run()
