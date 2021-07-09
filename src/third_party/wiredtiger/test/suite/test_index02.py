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
# [TEST_TAGS]
# indexes:search_near
# [END_TAGS]

import wiredtiger, wttest
from wtscenario import make_scenarios

def cmp(a, b):
    if a > b:
        return 1
    elif b > a:
        return -1
    else:
        return 0

# test_index02.py
#    test search_near in indices
class test_index02(wttest.WiredTigerTestCase):
    '''Test search_near in indices'''

    scenarios = make_scenarios([
        ('index', dict(indexconfig='columns=(v)', ncol=1)),
        ('index-with-key', dict(indexconfig='columns=(v,k)', ncol=2)),
    ])

    basename = 'test_index02'
    tablename = 'table:' + basename
    indexname = 'index:' + basename + ":inverse"

    def test_search_near_exists(self):
        '''Create a table, look for an existing key'''
        self.session.create(self.tablename, 'key_format=r,value_format=Q,columns=(k,v)')
        self.session.create(self.indexname, self.indexconfig)
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
        cur.close()

        # Retry after reopening
        for runs in range(2):
            # search near should find a match
            cur = self.session.open_cursor(self.indexname, None, None)
            if self.ncol == 1:
                cur.set_key(5)
            else:
                cur.set_key(5, 3)
            self.assertEqual(cur.search_near(), 0)

            # Retry after reopening
            self.reopen_conn()

    def test_search_near_between(self):
        '''Create a table, look for a non-existing key'''
        self.session.create(self.tablename, 'key_format=i,value_format=i,columns=(k,v)')
        self.session.create(self.indexname, self.indexconfig)
        cur = self.session.open_cursor(self.tablename)
        for k in range(3):
            cur[k] = 5 * k + 10
        cur.close()

        search_keys = [ 1, 11, 15, 19, 21 ]

        # search near should find a match
        for runs in range(2):
            cur = self.session.open_cursor(self.indexname, None, None)
            for k in search_keys:
                if self.ncol == 1:
                    cur.set_key(k)
                else:
                    cur.set_key(k, 1)    # [15,1] will completely match
                exact = cur.search_near()
                if self.ncol == 1:
                    found_key = cur.get_key()
                else:
                    [ found_key, index ] = cur.get_key()
                self.pr("search_near for " + str(k) + " found " + str(found_key) + " with exact " + str(exact))
                self.assertEqual(exact, cmp(found_key, k), "for key " + str(k))
            self.reopen_conn()

    def test_search_near_empty(self):
        '''Create an empty table, look for a key'''
        self.session.create(self.tablename, 'key_format=i,value_format=i,columns=(k,v)')
        self.session.create(self.indexname, self.indexconfig)

        cur = self.session.open_cursor(self.indexname, None, None)
        if self.ncol == 1:
            cur.set_key(3)
        else:
            cur.set_key(3, 1)
        self.assertEqual(cur.search_near(), wiredtiger.WT_NOTFOUND)

if __name__ == '__main__':
    wttest.run()
