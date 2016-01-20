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

# test_base03.py
#    Cursor operations
class test_base03(wttest.WiredTigerTestCase):
    """
    Test basic operations
    """
    table_name1 = 'test_base03a'
    table_name2 = 'test_base03b'
    table_name3 = 'test_base03c'
    table_name4 = 'test_base03d'
    nentries = 10

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

    def test_table_ss(self):
        """
        Create entries, and read back in a cursor: key=string, value=string
        """
        create_args = 'key_format=S,value_format=S' + self.config_string()
        self.session_create("table:" + self.table_name1, create_args)
        self.pr('creating cursor')
        cursor = self.session.open_cursor('table:' + self.table_name1, None, None)
        for i in range(0, self.nentries):
            cursor['key' + str(i)] = 'value' + str(i)

        i = 0
        cursor.reset()
        for key, value in cursor:
            self.assertEqual(key, ('key' + str(i)))
            self.assertEqual(value, ('value' + str(i)))
            i += 1

        self.assertEqual(i, self.nentries)
        cursor.close()

    def test_table_si(self):
        """
        Create entries, and read back in a cursor: key=string, value=int
        """
        create_args = 'key_format=S,value_format=i' + self.config_string()
        self.session_create("table:" + self.table_name2, create_args)
        self.pr('creating cursor')
        cursor = self.session.open_cursor('table:' + self.table_name2, None, None)
        for i in range(0, self.nentries):
            cursor['key' + str(i)] = i

        i = 0
        cursor.reset()
        for key, value in cursor:
            self.pr('got: ' + str(key) + ': ' + str(value))
            self.assertEqual(key, 'key' + str(i))
            self.assertEqual(value, i)
            i += 1

        self.pr("i = " + str(i))
        self.pr("self.... = " + str(self.nentries))
        self.assertEqual(i, self.nentries)
        cursor.close()

    def test_table_is(self):
        """
        Create entries, and read back in a cursor: key=int, value=string
        """
        create_args = 'key_format=i,value_format=S' + self.config_string()
        self.session_create("table:" + self.table_name3, create_args)
        self.pr('creating cursor')
        cursor = self.session.open_cursor('table:' + self.table_name3, None, None)
        for i in range(0, self.nentries):
            cursor[i] = 'value' + str(i)

        i = 0
        cursor.reset()
        for key, value in cursor:
            self.pr('got: ' + str(key) + ': ' + str(value))
            self.assertEqual(key, i)
            self.assertEqual(value, 'value' + str(i))
            i += 1

        self.assertEqual(i, self.nentries)
        cursor.close()

    def test_table_ii(self):
        """
        Create entries, and read back in a cursor: key=int, value=int
        """
        create_args = 'key_format=i,value_format=i' + self.config_string()
        self.session_create("table:" + self.table_name4, create_args)
        self.pr('creating cursor')
        cursor = self.session.open_cursor('table:' + self.table_name4, None, None)
        self.pr('stepping')
        for i in range(0, self.nentries):
            self.pr('put %d -> %d' % (i, i))
            cursor[i] = i

        i = 0
        cursor.reset()
        for key, value in cursor:
            self.pr('got %d -> %d' % (key, value))
            self.assertEqual(key, i)
            self.assertEqual(value, i)
            i += 1

        self.assertEqual(i, self.nentries)
        cursor.close()

if __name__ == '__main__':
    wttest.run()
