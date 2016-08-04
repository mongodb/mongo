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

# test_index02.py
#    test search_near in indices
class test_index02(wttest.WiredTigerTestCase):
    '''Test search_near in indices'''

    basename = 'test_index02'
    tablename = 'table:' + basename
    indexname = 'index:' + basename + ":inverse"

    def test_search_near(self):
        '''Create a table, look for a nonexistent key'''
        self.session.create(self.tablename, 'key_format=r,value_format=Q,columns=(k,v)')
        self.session.create(self.indexname, 'columns=(v)')
        cur = self.session.open_cursor(self.tablename, None, "append")
        cur.set_value(1)
        cur.insert()
        cur.set_value(5)
        cur.insert()
        cur.set_value(5)
        cur.insert()
        cur.set_value(5)
        cur.insert()
        cur.set_value(10)
        cur.insert()

        # search near should find a match
        cur2 = self.session.open_cursor(self.indexname, None, None)
        cur2.set_key(5)
        self.assertEqual(cur2.search_near(), 0)

        # Retry after reopening
        self.reopen_conn()
        cur3 = self.session.open_cursor(self.indexname, None, None)
        cur3.set_key(5)
        self.assertEqual(cur3.search_near(), 0)

if __name__ == '__main__':
    wttest.run()
