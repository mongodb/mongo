#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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

# test_drop_create.py
#    Test dropping and creating
class test_drop_create(wttest.WiredTigerTestCase):
    def test_drop_create(self):
        s, self.session = self.session, None
        self.assertEqual(s.close(), 0)

        for config in [None, 'key_format=S,value_format=S', None]:
            s = self.conn.open_session()
            self.assertEqual(s.drop("table:test", "force"), 0)
            self.assertEqual(s.create("table:test", config), 0)
            self.assertEqual(s.drop("table:test"), 0)
            self.assertEqual(s.close(), 0)
            s = self.conn.open_session()
            self.assertNotEqual(s, None)
            self.assertEqual(s.create("table:test", config), 0)
            self.assertEqual(s.close(), 0)

    def test_drop_create2(self):
        s, self.session = self.session, None
        self.assertEqual(s.close(), 0)

        # Test creating the same table with multiple sessions, to ensure
        # that session table cache is working as expected.
        s = self.conn.open_session()
        s2 = self.conn.open_session()
        self.assertEqual(s.drop("table:test", "force"), 0)
        self.assertEqual(s.create("table:test", 'key_format=S,value_format=S,columns=(k,v)'), 0)
        # Ensure the table cache for the second session knows about this table
        c2 = s2.open_cursor("table:test", None, None)
        c2.close()
        self.assertEqual(s.drop("table:test"), 0)
        # Create a table with the same name, but a different schema
        self.assertEqual(s.create("table:test", 'key_format=S,value_format=l,columns=(k,v)'), 0)
        c2 = s2.open_cursor("table:test", None, None)
        c2["Hi"] = 1
        self.assertEqual(s.close(), 0)
        self.assertEqual(s2.close(), 0)

if __name__ == '__main__':
    wttest.run()
