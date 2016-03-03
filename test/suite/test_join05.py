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
from wtscenario import check_scenarios, multiply_scenarios, number_scenarios

# test_join05.py
#    Tests based on JIRA reports
class test_join05(wttest.WiredTigerTestCase):

    # test join having the first index just be lt/le
    def test_wt_2384(self):
        self.session.create("table:test_2384",
                       "key_format=i,value_format=i,columns=(k,v)")
        self.session.create("index:test_2384:index", "columns=(v)")
        cursor = self.session.open_cursor("table:test_2384", None, None)
        cursor[1] = 11
        cursor[2] = 12
        cursor[3] = 13
        cursor.close()

        cursor = self.session.open_cursor("index:test_2384:index", None, None)
        cursor.set_key(13)
        self.assertEquals(cursor.search(), 0)

        jcursor = self.session.open_cursor("join:table:test_2384", None, None)
        self.session.join(jcursor, cursor, "compare=lt")

        nr_found = 0
        while jcursor.next() == 0:
            [k] = jcursor.get_keys()
            [v] = jcursor.get_values()
            #self.tty("jcursor: k=" + str(k) + ", v=" + str(v))
            nr_found += 1

        self.assertEquals(nr_found, 2)
        jcursor.close()
        cursor.close()

if __name__ == '__main__':
    wttest.run()
