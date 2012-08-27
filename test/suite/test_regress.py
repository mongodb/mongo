#!/usr/bin/env python
#
# Public Domain 2008-2012 WiredTiger, Inc.
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
# test_regress.py
#       Regression tests.
#

import wiredtiger, wttest

# Regression tests.
class test_regression(wttest.WiredTigerTestCase):

    # Test a bug where cursor movement inside implicit records failed.
    def test_regression_0001(self):
        uri='file:xxx'
        config='leaf_page_max=512,value_format=8t,key_format=r'
        self.session.create(uri, config)

        # Insert 50 records, and 20 implicit records.
        cursor = self.session.open_cursor(uri, None)
        for i in range(1, 50):
            cursor.set_key(i)
            cursor.set_value(0xab)
            cursor.insert()

        cursor.set_key(70)
        cursor.set_value(0xbb)
        cursor.insert()

        # Check search inside the implicit keys.
        for i in range(0, 5):
                cursor.set_key(60 + i)
                cursor.search()
                self.assertEqual(cursor.get_key(), 60 + i)
                self.assertEqual(cursor.get_value(), 0x00)

        # Check cursor next inside the implicit keys.
        cursor.set_key(60)
        cursor.search()
        for i in range(0, 5):
                self.assertEqual(cursor.get_key(), 60 + i)
                self.assertEqual(cursor.get_value(), 0x00)
                cursor.next()

        # Check cursor prev inside the implicit keys.
        cursor.set_key(60)
        cursor.search()
        for i in range(0, 5):
                self.assertEqual(cursor.get_key(), 60 - i)
                self.assertEqual(cursor.get_value(), 0x00)
                cursor.prev()


    # Test a bug where cursor remove inside implicit records looped infinitely.
    def test_regression_0002(self):
        uri='file:xxx'
        config='leaf_page_max=512,value_format=8t,key_format=r'
        self.session.create(uri, config)

        # Insert 50 records, and 20 implicit records.
        cursor = self.session.open_cursor(uri, None)
        for i in range(1, 50):
            cursor.set_key(i)
            cursor.set_value(0xab)
            cursor.insert()

        cursor.set_key(70)
        cursor.set_value(0xbb)
        cursor.insert()

        cursor.set_key(68)
        cursor.search()
        for i in range(1, 5):
            cursor.prev()
            self.assertEqual(cursor.get_key(), 68 - i)
            self.assertEqual(cursor.get_value(), 0x00)
            cursor.remove()

        cursor.set_key(62)
        cursor.search()
        for i in range(1, 5):
            cursor.next()
            self.assertEqual(cursor.get_key(), 62 + i)
            self.assertEqual(cursor.get_value(), 0x00)
            cursor.remove()


if __name__ == '__main__':
    wttest.run()
