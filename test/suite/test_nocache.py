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

import wiredtiger, wttest
from helper import simple_populate, key_populate, value_populate

# Test no-cache flag.
class test_no_cache(wttest.WiredTigerTestCase):
    name = 'no_cache'

    scenarios = [
        ('file', dict(type='file:')),
        ('table', dict(type='table:'))
    ]

    # Create an object, and run an uncached cursor through it.
    def test_no_cache(self):
        uri = self.type + self.name
        simple_populate(self, uri, 'key_format=S,leaf_page_max=512', 10000)
        cursor = self.session.open_cursor(uri, None, "no_cache")
        i = 0
        for key,val in cursor:
            i += 1
            self.assertEqual(key, key_populate(cursor, i))
            self.assertEqual(val, value_populate(cursor, i))

    # Create an object, and run an uncached cursor through part of it to
    # confirm that we release the full stack on an uncached cursor.
    def test_no_cache_partial(self):
        uri = self.type + self.name
        simple_populate(self, uri, 'key_format=S,leaf_page_max=512', 10000)
        cursor = self.session.open_cursor(uri, None, "no_cache")
        i = 0
        for key,val in cursor:
            i += 1
            if i > 2000:
                break;
            self.assertEqual(key, key_populate(cursor, i))
            self.assertEqual(val, value_populate(cursor, i))
        cursor.close()

    # Uncached cursors are exclusive objects, other opens must fail.
    def test_no_cache_handle_block(self):
        uri = self.type + self.name
        simple_populate(self, uri, 'key_format=S', 100)
        cursor = self.session.open_cursor(uri, None, "no_cache")
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(uri, None))


if __name__ == '__main__':
    wttest.run()

