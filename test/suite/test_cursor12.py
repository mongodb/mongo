#!/usr/bin/env python
#
# Public Domain 2014-2017 MongoDB, Inc.
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

# test_cursor12.py
#    Test cursor modify call
class test_cursor12(wttest.WiredTigerTestCase):
    types = [
        ('file', dict(uri='file:modify')),
        ('lsm', dict(uri='lsm:modify')),
        ('table', dict(uri='table:modify')),
    ]
    scenarios = make_scenarios(types)

    # Smoke-test the modify API.
    def test_modify_smoke(self):
        # List with original value, final value, and modifications to get
        # there.
        list = [
        {
        'o' : 'ABCDEFGH',           # no operation
        'f' : 'ABCDEFGH',
        'mods' : [['', 0, 0]]
        },{
        'o' : 'ABCDEFGH',           # no operation with offset
        'f' : 'ABCDEFGH',
        'mods' : [['', 4, 0]]
        },{
        'o' : 'ABCDEFGH',           # rewrite beginning
        'f' : '--CDEFGH',
        'mods' : [['--', 0, 2]]
        },{
        'o' : 'ABCDEFGH',           # rewrite end
        'f' : 'ABCDEF--',
        'mods' : [['--', 6, 2]]
        },{
        'o' : 'ABCDEFGH',           # append
        'f' : 'ABCDEFGH--',
        'mods' : [['--', 8, 2]]
        },{
        'o' : 'ABCDEFGH',           # append with gap
        'f' : 'ABCDEFGH\00\00--',
        'mods' : [['--', 10, 2]]
        },{
        'o' : 'ABCDEFGH',           # multiple replacements
        'f' : 'A-C-E-G-',
        'mods' : [['-', 1, 1], ['-', 3, 1], ['-', 5, 1], ['-', 7, 1]]
        },{
        'o' : 'ABCDEFGH',           # multiple overlapping replacements
        'f' : 'A-CDEFGH',
        'mods' : [['+', 1, 1], ['+', 1, 1], ['+', 1, 1], ['-', 1, 1]]
        },{
        'o' : 'ABCDEFGH',           # multiple overlapping gap replacements
        'f' : 'ABCDEFGH\00\00--',
        'mods' : [['+', 10, 1], ['+', 10, 1], ['+', 10, 1], ['--', 10, 2]]
        },{
        'o' : 'ABCDEFGH',           # shrink beginning
        'f' : '--EFGH',
        'mods' : [['--', 0, 4]]
        },{
        'o' : 'ABCDEFGH',           # shrink middle
        'f' : 'AB--GH',
        'mods' : [['--', 2, 4]]
        },{
        'o' : 'ABCDEFGH',           # shrink end
        'f' : 'ABCD--',
        'mods' : [['--', 4, 4]]
        },{
        'o' : 'ABCDEFGH',           # grow beginning
        'f' : '--ABCDEFGH',
        'mods' : [['--', 0, 0]]
        },{
        'o' : 'ABCDEFGH',           # grow middle
        'f' : 'ABCD--EFGH',
        'mods' : [['--', 4, 0]]
        },{
        'o' : 'ABCDEFGH',           # grow end
        'f' : 'ABCDEFGH--',
        'mods' : [['--', 8, 0]]
        },{
        'o' : 'ABCDEFGH',           # discard beginning
        'f' : 'EFGH',
        'mods' : [['', 0, 4]]
        },{
        'o' : 'ABCDEFGH',           # discard middle
        'f' : 'ABGH',
        'mods' : [['', 2, 4]]
        },{
        'o' : 'ABCDEFGH',           # discard end
        'f' : 'ABCD',
        'mods' : [['', 4, 4]]
        },{
        'o' : 'ABCDEFGH',           # overlap the end and append
        'f' : 'ABCDEF--XX',
        'mods' : [['--XX', 6, 2]]
        },{
        'o' : 'ABCDEFGH',           # overlap the end with incorrect size
        'f' : 'ABCDEFG01234567',
        'mods' : [['01234567', 7, 2000]]
        }
        ]

        self.session.create(self.uri, 'key_format=S,value_format=u')
        cursor = self.session.open_cursor(self.uri, None, None)

        # For each test in the list, set the original value, apply modifications
        # in order, then confirm the final state.
        for i in list:
            cursor['ABC'] = i['o']

            mods = []
            for j in i['mods']:
                mod = wiredtiger.Modify(j[0], j[1], j[2])
                mods.append(mod)

            cursor.set_key('ABC')
            cursor.modify(mods)
            self.assertEquals(str(cursor['ABC']), i['f'])

    # Check that modify returns not-found after a delete.
    def test_modify_delete(self):
        self.session.create(self.uri, 'key_format=S,value_format=u')
        cursor = self.session.open_cursor(self.uri, None, None)
        cursor['ABC'] = 'ABCDEFGH'
        cursor.set_key('ABC')
        cursor.remove()

        mods = []
        mod = wiredtiger.Modify('ABCD', 3, 3)
        mods.append(mod)

        cursor.set_key('ABC')
        #self.assertEqual(cursor.modify(mods), wiredtiger.WT_NOTFOUND)
        self.assertRaises(
            wiredtiger.WiredTigerError, lambda:cursor.modify(mods))

if __name__ == '__main__':
    wttest.run()
