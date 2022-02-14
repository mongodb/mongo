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
#
# test_bug001.py
#       Regression tests.

import wttest

# Regression tests.
class test_bug001(wttest.WiredTigerTestCase):

    def create_implicit(self, uri, initial, middle, trailing):
        self.session.create(uri, 'key_format=r,value_format=8t')
        cursor = self.session.open_cursor(uri, None)

        # Create a set of initial implicit records, followed by a set of real
        # records, followed by a set of trailing implicit records.
        r = initial
        for i in range(0, middle):
            r += 1
            cursor[r] = 0xab
        r += trailing
        cursor[r + 1] = 0xbb
        return (cursor)

    # Test a bug where cursor movement inside implicit records failed.
    def test_implicit_record_cursor_movement(self):
        uri = 'file:xxx'
        cursor = self.create_implicit(uri, 0, 50, 20)

        # Check search inside trailing implicit keys.
        for i in range(0, 5):
            self.assertEqual(cursor[60 + i], 0x00)

        # Check cursor next inside trailing implicit keys.
        cursor.set_key(60)
        self.assertEquals(cursor.search(), 0)
        for i in range(0, 5):
            self.assertEqual(cursor.get_key(), 60 + i)
            self.assertEqual(cursor.get_value(), 0x00)
            self.assertEqual(cursor.next(), 0)

        # Check cursor prev inside trailing implicit keys.
        cursor.set_key(60)
        self.assertEquals(cursor.search(), 0)
        for i in range(0, 5):
            self.assertEqual(cursor.get_key(), 60 - i)
            self.assertEqual(cursor.get_value(), 0x00)
            self.assertEqual(cursor.prev(), 0)

        self.assertEquals(cursor.close(), 0)
        self.dropUntilSuccess(self.session, uri)
        cursor = self.create_implicit(uri, 20, 50, 0)

        # Check search inside leading implicit keys.
        for i in range(0, 5):
            self.assertEqual(cursor[10 + i], 0x00)

        # Check cursor next inside leading implicit keys.
        cursor.set_key(10)
        self.assertEquals(cursor.search(), 0)
        for i in range(0, 5):
            self.assertEqual(cursor.get_key(), 10 + i)
            self.assertEqual(cursor.get_value(), 0x00)
            self.assertEqual(cursor.next(), 0)

        # Check cursor prev inside leading implicit keys.
        cursor.set_key(10)
        self.assertEquals(cursor.search(), 0)
        for i in range(0, 5):
            self.assertEqual(cursor.get_key(), 10 - i)
            self.assertEqual(cursor.get_value(), 0x00)
            self.assertEqual(cursor.prev(), 0)

        self.assertEquals(cursor.close(), 0)
        self.dropUntilSuccess(self.session, uri)

    # Test a bug where cursor remove inside implicit records looped infinitely.
    def test_implicit_record_cursor_remove(self):
        uri='file:xxx'
        cursor = self.create_implicit(uri, 0, 50, 20)

        # Check cursor next/remove inside trailing implicit keys.
        cursor.set_key(62)
        self.assertEquals(cursor.search(), 0)
        for i in range(1, 5):
            self.assertEquals(cursor.next(), 0)
            self.assertEqual(cursor.get_key(), 62 + i)
            self.assertEqual(cursor.get_value(), 0x00)
            self.assertEquals(cursor.remove(), 0)

        # Check cursor prev/remove inside trailing implicit keys.
        cursor.set_key(68)
        self.assertEquals(cursor.search(), 0)
        for i in range(1, 5):
            self.assertEquals(cursor.prev(), 0)
            self.assertEqual(cursor.get_key(), 68 - i)
            self.assertEqual(cursor.get_value(), 0x00)
            self.assertEquals(cursor.remove(), 0)

        self.assertEquals(cursor.close(), 0)
        self.dropUntilSuccess(self.session, uri)
        cursor = self.create_implicit(uri, 20, 50, 0)

        # Check cursor next/remove inside leading implicit keys.
        cursor.set_key(2)
        self.assertEquals(cursor.search(), 0)
        for i in range(1, 5):
            self.assertEquals(cursor.next(), 0)
            self.assertEqual(cursor.get_key(), 2 + i)
            self.assertEqual(cursor.get_value(), 0x00)
            self.assertEquals(cursor.remove(), 0)

        # Check cursor prev/remove inside leading implicit keys.
        cursor.set_key(18)
        self.assertEquals(cursor.search(), 0)
        for i in range(1, 5):
            self.assertEquals(cursor.prev(), 0)
            self.assertEqual(cursor.get_key(), 18 - i)
            self.assertEqual(cursor.get_value(), 0x00)
            self.assertEquals(cursor.remove(), 0)

        self.assertEquals(cursor.close(), 0)
        self.dropUntilSuccess(self.session, uri)

if __name__ == '__main__':
    wttest.run()
