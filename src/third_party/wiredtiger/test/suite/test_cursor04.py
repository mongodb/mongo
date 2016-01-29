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
from wtscenario import check_scenarios

# test_base04.py
#     Cursor operations
class test_cursor04(wttest.WiredTigerTestCase):
    """
    Test cursor search and search_near
    """
    table_name1 = 'test_cursor04'
    nentries = 20

    scenarios = check_scenarios([
        ('row', dict(tablekind='row', uri='table')),
        ('lsm-row', dict(tablekind='row', uri='lsm')),
        ('col', dict(tablekind='col', uri='table')),
        ('fix', dict(tablekind='fix', uri='table'))
    ])

    def config_string(self):
        """
        Return any additional configuration.
        This method may be overridden.
        """
        return ''

    def session_create(self, name, args):
        """
        session.create, but report errors more completely
        """
        try:
            self.session.create(name, args)
        except:
            print('**** ERROR in session.create("' + name + '","' + args + '") ***** ')
            raise

    def create_session_and_cursor(self):
        tablearg = self.uri + ":" + self.table_name1
        if self.tablekind == 'row':
            keyformat = 'key_format=S'
        else:
            keyformat = 'key_format=r'  # record format
        if self.tablekind == 'fix':
            valformat = 'value_format=8t'
        else:
            valformat = 'value_format=S'
        create_args = keyformat + ',' + valformat + self.config_string()
        self.pr('creating session: ' + create_args)
        self.session_create(tablearg, create_args)
        self.pr('creating cursor')
        return self.session.open_cursor(tablearg, None, None)

    def genkey(self, i):
        if self.tablekind == 'row':
            return 'key' + str(i).zfill(5)  # return key00001, key00002, etc.
        else:
            return long(i+1)

    def genvalue(self, i):
        if self.tablekind == 'fix':
            return int(i & 0xff)
        else:
            return 'value' + str(i)

    def expect_either(self, cursor, lt, gt):
        origkey = cursor.get_key()
        direction = cursor.search_near()
        self.assertNotEqual(direction, wiredtiger.WT_NOTFOUND)

        # Deletions for 'fix' clear the value, they
        # do not remove the key, so we expect '0' direction
        # (that is key found) for fix.
        if self.tablekind != 'fix':
            self.assertTrue(direction == 1 or direction == -1)
        else:
            self.assertEqual(direction, 0)

        if direction == 1:
            self.assertEqual(cursor.get_key(), self.genkey(gt))
            self.assertEqual(cursor.get_value(), self.genvalue(gt))
        elif direction == -1:
            self.assertEqual(cursor.get_key(), self.genkey(lt))
            self.assertEqual(cursor.get_value(), self.genvalue(lt))
        else:
            self.assertEqual(direction, 0)
            self.assertEqual(cursor.get_key(), origkey)
            self.assertEqual(cursor.get_value(), 0)

    def test_searches(self):
        """
        Create entries, and read back in a cursor: key=string, value=string
        """
        cursor = self.create_session_and_cursor()

        # Some tests below expect keys between 0-10 to be available
        self.assertTrue(self.nentries > 10)

        # 0. Populate the key space
        for i in range(0, self.nentries):
            cursor[self.genkey(i)] = self.genvalue(i)

        # 1. Calling search for a value that exists
        self.assertEqual(cursor[self.genkey(5)], self.genvalue(5))

        # 2. Calling search for a value that does not exist
        self.assertRaises(KeyError, lambda: cursor[self.genkey(self.nentries)])

        # 2. Calling search_near for a value beyond the end
        cursor.set_key(self.genkey(self.nentries))
        cmp = cursor.search_near()
        self.assertEqual(cmp, -1)
        self.assertEqual(cursor.get_key(), self.genkey(self.nentries-1))
        self.assertEqual(cursor.get_value(), self.genvalue(self.nentries-1))

        # 2.a calling search_near for an existing value
        cursor.set_key(self.genkey(7))
        cmp = cursor.search_near()
        self.assertEqual(cmp, 0)
        self.assertEqual(cursor.get_key(), self.genkey(7))
        self.assertEqual(cursor.get_value(), self.genvalue(7))

        # 3. Delete some keys
        # Deletions for 'fix' clear the value, they
        # do not remove the key
        cursor.set_key(self.genkey(0))
        cursor.remove()
        cursor.set_key(self.genkey(5))
        cursor.remove()
        cursor.set_key(self.genkey(9))
        cursor.remove()
        cursor.set_key(self.genkey(10))
        cursor.remove()

        #cursor.reset()
        #for key, value in cursor:
        #    print('key: ' + str(key))
        #    print('value: ' + str(value))

        cursor.set_key(self.genkey(0))
        cmp = cursor.search_near()
        if self.tablekind != 'fix':
            self.assertEqual(cmp, 1)
            self.assertEqual(cursor.get_key(), self.genkey(1))
            self.assertEqual(cursor.get_value(), self.genvalue(1))
        else:
            self.assertEqual(cmp, 0)
            self.assertEqual(cursor.get_key(), self.genkey(0))
            self.assertEqual(cursor.get_value(), 0)

        cursor.set_key(self.genkey(5))
        self.expect_either(cursor, 4, 6)

        cursor.set_key(self.genkey(9))
        self.expect_either(cursor, 8, 11)

        cursor.set_key(self.genkey(10))
        self.expect_either(cursor, 8, 11)

        cursor.close()

if __name__ == '__main__':
    wttest.run()
