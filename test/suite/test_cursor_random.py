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
from helper import complex_populate, simple_populate

# test_cursor_random.py
#    Cursor next_random operations
class test_cursor_random(wttest.WiredTigerTestCase):
    scenarios = [
        ('file', dict(type='file:',fmt='S')),
        ('table', dict(type='table:',fmt='S'))
        ]

    # Check that opening a random cursor on a row-store returns not-supported
    # for every method except for next and reset, and next returns not-found.
    def test_cursor_random_column(self):
        uri = self.type + 'random'
        self.session.create(uri, 'key_format=' + self.fmt + ',value_format=S')
        cursor = self.session.open_cursor(uri, None, "next_random=true")
        self.assertRaises(
            wiredtiger.WiredTigerError, lambda: cursor.compare(cursor))
        self.assertRaises(wiredtiger.WiredTigerError, lambda: cursor.prev())
        self.assertRaises(wiredtiger.WiredTigerError, lambda: cursor.search())
        self.assertRaises(
            wiredtiger.WiredTigerError, lambda: cursor.search_near())
        self.assertRaises(wiredtiger.WiredTigerError, lambda: cursor.insert())
        self.assertRaises(wiredtiger.WiredTigerError, lambda: cursor.update())
        self.assertRaises(wiredtiger.WiredTigerError, lambda: cursor.remove())

        cursor.reset()
        self.assertTrue(cursor.next(), wiredtiger.WT_NOTFOUND)
        cursor.close()

    # Check that next_random works with a single value, repeatedly.
    def test_cursor_random_single_record(self):
        uri = self.type + 'random'
        self.session.create(uri, 'key_format=' + self.fmt + ',value_format=S')
        cursor = self.session.open_cursor(uri, None)
        cursor.set_key('AAA')
        cursor.set_value('BBB')
        cursor.insert()
        cursor.close
        cursor = self.session.open_cursor(uri, None, "next_random=true")
        for i in range(1,5):
            cursor.next()
            self.assertEquals(cursor.get_key(), 'AAA')
        cursor.close

    # Check that next_random works in the presence of a larger set of values.
    def test_cursor_random_multiple_records(self):
        uri = self.type + 'random'
        if self.type == 'file:':
            simple_populate(
                self, uri, 'leaf_page_max=512,key_format=' + self.fmt, 10000)
        else:
            complex_populate(
                self, uri, 'leaf_page_max=512,key_format=' + self.fmt, 10000)

        # Close the connection so everything is forced to disk (otherwise the
        # values are on an insert list and the underlying engine doesn't make
        # random selections, it selects the middle of the list.
        self.conn.close()
        self.conn = self.setUpConnectionOpen(".")
        self.session = self.conn.open_session()
        cursor = self.session.open_cursor(uri, None, "next_random=true")
        last = ''
        match = 0
        for i in range(1,10):
            cursor.next()
            current = cursor.get_key()
            if current == last:
                match += 1
            last = current
        self.assertLess(match, 5,
            'next_random did not return random records, too many matches found')


# Check that opening a random cursor on column-store returns not-supported.
class test_cursor_random_column(wttest.WiredTigerTestCase):
    scenarios = [
        ('file', dict(uri='file:random',fmt='r')),
        ('table', dict(uri='table:random',fmt='r')),
        ]

    def test_cursor_random_column(self):
        self.session.create(
            self.uri, 'key_format=' + self.fmt + ',value_format=S')
        cursor = self.session.open_cursor(self.uri, None, "next_random=true")
        self.assertRaises(wiredtiger.WiredTigerError, lambda: cursor.next())
        cursor.close()


if __name__ == '__main__':
    wttest.run()
