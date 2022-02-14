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

import wttest
from wtscenario import make_scenarios

# test_schema04.py
#    Test indices with duplicates
class test_schema04(wttest.WiredTigerTestCase):
    """
    Test indices with duplicates.
    Our set of rows looks like a multiplication table:
      row 0:  [ 0, 0, 0, 0, 0, 0 ]
      row 1:  [ 0, 1, 2, 3, 4, 5 ]
      row 2:  [ 0, 2, 4, 6, 8, 10 ]
    with the twist that entries are mod 100.  So, looking further:
      row 31:  [ 0, 31, 62, 93, 24, 55 ]

    Each column is placed into its own index.  The mod twist,
    as well as the 0th column, guarantees we'll have some duplicates.
    """
    nentries = 100

    scenarios = make_scenarios([
        ('index-before', { 'create_index' : 0 }),
        ('index-during', { 'create_index' : 1 }),
        ('index-after', { 'create_index' : 2 }),
    ])

    def create_indices(self):
        # Create 6 index files, each with a column from the main table
        for i in range(0, 6):
            self.session.create("index:schema04:x" + str(i),
                                "key_format=i,columns=(v" + str(i) + "),")

    # We split the population into two phases
    # (in anticipation of future tests that create
    # indices between the two population steps).
    def populate(self, phase):
        cursor = self.session.open_cursor('table:schema04', None, None)
        if phase == 0:
            range_from = 0
            range_to = self.nentries // 2
        else:
            range_from = self.nentries // 2
            range_to = self.nentries

        for i in range(range_from, range_to):
            # e.g. element 31 is '0,31,62,93,24,55'
            cursor.set_key(i)
            cursor.set_value(
                (i*0)%100, (i*1)%100, (i*2)%100,
                (i*3)%100, (i*4)%100, (i*5)%100)
            cursor.insert()
        cursor.close()

    def check_entries(self):
        cursor = self.session.open_cursor('table:schema04', None, None)
        icursor = []
        for i in range(0, 6):
            icursor.append(self.session.open_cursor('index:schema04:x' + str(i),
                                                    None, None))
        i = 0
        for kv in cursor:
            # Check main table
            expect = [(i*j)%100 for j in range(0, 6)]
            primkey = kv.pop(0)
            self.assertEqual(i, primkey)
            self.assertEqual(kv, expect)
            for j in range(0, 6):
                self.assertEqual((i*j)%100, kv[j])
            for idx in range(0, 6):
                c = icursor[idx]
                indexkey = (i*idx)%100
                c.set_key(indexkey)
                self.assertEqual(c.search(), 0)
                value = c.get_value()
                while value != expect and value[idx] == expect[idx]:
                    c.next()
                    value = c.get_value()
                self.assertEqual(value, expect)
            i += 1
        self.assertEqual(self.nentries, i)

    def test_index(self):
        self.session.create("table:schema04",
                            "key_format=i,value_format=iiiiii,"
                            "columns=(primarykey,v0,v1,v2,v3,v4,v5)")
        if self.create_index == 0:
            self.create_indices()
        self.populate(0)
        if self.create_index == 1:
            self.create_indices()
        self.populate(1)
        if self.create_index == 2:
            self.create_indices()
        self.check_entries()

if __name__ == '__main__':
    wttest.run()
