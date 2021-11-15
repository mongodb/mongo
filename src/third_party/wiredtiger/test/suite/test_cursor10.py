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

import wiredtiger, wttest
from wtscenario import make_scenarios

# test_cursor10.py
#     Cursors with projections.
class test_cursor10(wttest.WiredTigerTestCase):
    """
    Test cursor search
    """
    table_name1 = 'test_cursor10'
    nentries = 20

    scenarios = make_scenarios([
        ('row', dict(key_format='S', uri='table')),
        ('col', dict(key_format='r', uri='table'))
    ])

    def genkey(self, i):
        if self.key_format == 'S':
            return 'key' + str(i).zfill(5)  # return key00001, key00002, etc.
        else:
            return self.recno(i+1)

    def genvalue(self, i):
        return [ 'v0:' + str(i), i+1, 'v2' + str(i+2), i+3 ]

    def extractkey(self, k):
        if self.key_format == 'S':
            return int(k[3:])
        else:
            return self.recno(k-1)

    def test_projection(self):
        """
        Create entries, and read back in a regular and projected cursor
        """
        tablearg = self.uri + ":" + self.table_name1
        create_args = 'columns=(k,v0,v1,v2,v3),value_format=SiSi,key_format=' \
                      + self.key_format
        self.session.create(tablearg, create_args)

        cursor = self.session.open_cursor(tablearg, None, None)
        for i in range(0, self.nentries):
            cursor.set_key(self.genkey(i))
            values = self.genvalue(i)
            cursor.set_value(*values)
            cursor.insert()
        cursor.close()
        cursor = self.session.open_cursor(tablearg, None, None)
        count = 0
        for k,v0,v1,v2,v3 in cursor:
            i = self.extractkey(k)
            self.assertEqual(self.genkey(i), k)
            self.assertEqual(self.genvalue(i), [v0,v1,v2,v3])
            count += 1
        self.assertEqual(count, self.nentries)
        cursor.close()
        cursor = self.session.open_cursor(tablearg + '(v3,v2,v1,v0,k)',\
                                          None, None)
        count = 0
        for k1,v3,v2,v1,v0,k2 in cursor:
            self.assertEqual(k1, k2)
            i = self.extractkey(k1)
            self.assertEqual(self.genkey(i), k1)
            self.assertEqual(self.genvalue(i), [v0,v1,v2,v3])
            count += 1
        self.assertEqual(count, self.nentries)
        cursor.close()

    def test_index_projection(self):
        """
        Create entries, and read back in an index cursor with a projection
        """
        tablearg = self.uri + ":" + self.table_name1
        indexarg = 'index:' + self.table_name1 + ':index1'
        create_args = 'columns=(k,v0,v1,v2,v3),value_format=SiSi,key_format=' \
                      + self.key_format
        self.session.create(tablearg, create_args)
        self.session.create(indexarg, 'columns=(v0,v2,v1,v3)')
        cursor = self.session.open_cursor(tablearg, None, None)
        for i in range(0, self.nentries):
            cursor.set_key(self.genkey(i))
            values = self.genvalue(i)
            cursor.set_value(*values)
            cursor.insert()
        cursor.close()
        cursor = self.session.open_cursor(tablearg + '(v3,v2,v1,v0,k)',\
                                          None, None)
        count = 0
        for k1,v3,v2,v1,v0,k2 in cursor:
            self.assertEqual(k1, k2)
            i = self.extractkey(k1)
            self.assertEqual(self.genkey(i), k1)
            self.assertEqual(self.genvalue(i), [v0,v1,v2,v3])
            count += 1
        self.assertEqual(count, self.nentries)
        cursor.close()
if __name__ == '__main__':
    wttest.run()
