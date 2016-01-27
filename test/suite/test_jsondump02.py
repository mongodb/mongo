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

import os
import wiredtiger, wttest

# test_jsondump.py
# Test dump output from json cursors.
class test_jsondump02(wttest.WiredTigerTestCase):

    table_uri1 = 'table:jsondump02a.wt'
    table_uri2 = 'table:jsondump02b.wt'
    table_uri3 = 'table:jsondump02c.wt'
    basename_uri4 = 'jsondump02d.wt'
    table_uri4 = 'table:' + basename_uri4

    def set_kv(self, uri, key, val):
        cursor = self.session.open_cursor(uri, None, None)
        try:
            cursor[key] = val
        finally:
            cursor.close()

    def set_kv2(self, uri, key, val1, val2):
        cursor = self.session.open_cursor(uri, None, None)
        try:
            cursor[key] = (val1, val2)
        finally:
            cursor.close()

    # Populate table_uri4 with squares and cubes of row numbers
    def populate_squarecube(self, uri):
        cursor = self.session.open_cursor(uri, None, None)
        for i in range(1, 5):
            square = i * i
            cube = square * i
            cursor[(i, 'key' + str(i))] = \
                ('val' + str(square), square, 'val' + str(cube), cube)
        cursor.close()

    # Check the result of using a JSON cursor on the URI.
    def check_json(self, uri, expect):
        cursor = self.session.open_cursor(uri, None, 'dump=json')
        pos = 0
        for k,v in cursor:
            self.assertEqual(k, expect[pos][0])
            self.assertEqual(v, expect[pos][1])
            pos += 1
        self.assertEqual(pos, len(expect))
        cursor.close()

    # Check the result of using a JSON cursor on the URI.
    def load_json(self, uri, inserts):
        cursor = self.session.open_cursor(uri, None, 'dump=json')
        pos = 0
        try:
            for insert in inserts:
                #tty_pr('Insert: ' + str(insert))
                cursor[insert[0]] = insert[1]
        finally:
            cursor.close()

    # Create JSON cursors and test them directly.
    def test_json_cursor(self):
        """
        Create a table, add a key, get it back
        """
        extra_params = ',allocation_size=512,' +\
            'internal_page_max=16384,leaf_page_max=131072'
        self.session.create(self.table_uri1,
            'key_format=S,value_format=S' + extra_params)
        self.session.create(self.table_uri2,
            'key_format=S,value_format=8tS' + extra_params)
        self.session.create(self.table_uri3,
            'key_format=r,value_format=u' + extra_params)
        self.session.create(self.table_uri4,
                            "key_format=iS,value_format=SiSi,"
                            "columns=(ikey,Skey,S1,i2,S3,i4),colgroups=(c1,c2)")
        cg = 'colgroup:' + self.basename_uri4
        self.session.create(cg + ":c1", "columns=(S1,i2)")
        self.session.create(cg + ":c2", "columns=(S3,i4)")
        uri4index1 = 'index:' + self.basename_uri4 + ':by-Skey'
        uri4index2 = 'index:' + self.basename_uri4 + ':by-S3'
        uri4index3 = 'index:' + self.basename_uri4 + ':by-i2i4'
        self.session.create(uri4index1, "columns=(Skey)")
        self.session.create(uri4index2, "columns=(S3)")
        self.session.create(uri4index3, "columns=(i2,i4)")

        self.set_kv(self.table_uri1, 'KEY000', 'string value')
        self.set_kv(self.table_uri1, 'KEY001', '\'\"({[]})\"\', etc. allowed')
        self.set_kv2(self.table_uri2, 'KEY000', 123, 'str0')
        self.set_kv2(self.table_uri2, 'KEY001', 234, 'str1')
        self.set_kv(self.table_uri3, 1, '\x01\x02\x03')
        self.set_kv(self.table_uri3, 2, '\x77\x88\x99\x00\xff\xfe')
        self.populate_squarecube(self.table_uri4)

        table1_json =  (
            ('"key0" : "KEY000"', '"value0" : "string value"'),
            ('"key0" : "KEY001"', '"value0" : ' +
             '"\'\\\"({[]})\\\"\', etc. allowed"'))
        self.check_json(self.table_uri1, table1_json)

        self.session.truncate(self.table_uri1, None, None, None)
        self.load_json(self.table_uri1, table1_json)
        self.check_json(self.table_uri1, table1_json)

        table2_json =  (
            ('"key0" : "KEY000"', '"value0" : 123,\n"value1" : "str0"'),
            ('"key0" : "KEY001"', '"value0" : 234,\n"value1" : "str1"'))
        self.check_json(self.table_uri2, table2_json)
        self.session.truncate(self.table_uri2, None, None, None)
        self.load_json(self.table_uri2, table2_json)
        self.check_json(self.table_uri2, table2_json)
        self.session.truncate(self.table_uri2, None, None, None)

        # bad tokens
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.load_json(self.table_uri2,
              (('<>abc?', '9'),)),
            '/unknown token/')

        # bad tokens
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.load_json(self.table_uri2,
              (('"abc\u"', ''),)),
            '/invalid Unicode/')

        # bad tokens
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.load_json(self.table_uri2,
              (('"abc', ''),)),
            '/unterminated string/')

        # bad syntax
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.load_json(self.table_uri2,
              (('"stuff" "jibberish"', '"value0" "more jibberish"'),)),
            '/expected key name.*\"key0\"/')

        # bad types
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.load_json(self.table_uri2,
              (('"key0" : "KEY002"', '"value0" : "xyz",\n"value1" : "str0"'),)),
            '/expected unsigned JSON <int>, got <string>/')

        # bad types
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.load_json(self.table_uri2,
              (('"key0" : "KEY002"', '"value0" : 123,\n"value1" : 456'),)),
            '/expected JSON <string>, got <integer>/')

        # extra stuff
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.load_json(self.table_uri2,
              (('"key0" : "KEY002"',
                '"value0" : 123,\n"value1" : "str0",'),)),
            '/expected JSON <EOF>, got \',\'/')

        # fields out of order currently not supported
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.load_json(self.table_uri2,
              (('"key0" : "KEY002"', '"value1" : "str0",\n"value0" : 123'),)),
            '/expected value name.*\"value0\"/')

        # various invalid unicode
        invalid_unicode = (
            '\\u', '\\ux', '\\u0', '\\u0F', '\\u0FA', '\\u0FAx',  '\\u0FA\\x')
        for uni in invalid_unicode:
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.load_json(self.table_uri2,
                  (('"key0" : "KEY002"', '"value0" : 123,\n"value1" : "'
                    + uni + '"'),)),
                '/invalid Unicode/')

        # this one should work
        self.load_json(self.table_uri2,
              (('"key0" : "KEY002"', '"value0" : 345,\n"value1" : "str2"'),))

        # extraneous/missing space is okay
        self.load_json(self.table_uri2,
              (('  "key0"\n:\t"KEY003"    ',
                '"value0":456,"value1"\n\n\r\n:\t\n"str3"'),))

        self.check_json(self.table_uri3, (
                ('"key0" : 1', '"value0" : "\\u0001\\u0002\\u0003"'),
                ('"key0" : 2',
                 '"value0" : "\\u0077\\u0088\\u0099\\u0000\\u00ff\\u00fe"')))
        self.check_json(self.table_uri4, (
                ('"ikey" : 1,\n"Skey" : "key1"',
                 '"S1" : "val1",\n"i2" : 1,\n"S3" : "val1",\n"i4" : 1'),
                ('"ikey" : 2,\n"Skey" : "key2"',
                 '"S1" : "val4",\n"i2" : 4,\n"S3" : "val8",\n"i4" : 8'),
                ('"ikey" : 3,\n"Skey" : "key3"',
                 '"S1" : "val9",\n"i2" : 9,\n"S3" : "val27",\n"i4" : 27'),
                ('"ikey" : 4,\n"Skey" : "key4"',
                 '"S1" : "val16",\n"i2" : 16,\n"S3" : "val64",\n"i4" : 64')))
        # The dump config currently is not supported for the index type.
        self.check_json(uri4index1, (
                ('"Skey" : "key1"',
                 '"S1" : "val1",\n"i2" : 1,\n"S3" : "val1",\n"i4" : 1'),
                ('"Skey" : "key2"',
                 '"S1" : "val4",\n"i2" : 4,\n"S3" : "val8",\n"i4" : 8'),
                ('"Skey" : "key3"',
                 '"S1" : "val9",\n"i2" : 9,\n"S3" : "val27",\n"i4" : 27'),
                ('"Skey" : "key4"',
                 '"S1" : "val16",\n"i2" : 16,\n"S3" : "val64",\n"i4" : 64')))
        self.check_json(uri4index2, (
                ('"S3" : "val1"',
                 '"S1" : "val1",\n"i2" : 1,\n"S3" : "val1",\n"i4" : 1'),
                ('"S3" : "val27"',
                 '"S1" : "val9",\n"i2" : 9,\n"S3" : "val27",\n"i4" : 27'),
                ('"S3" : "val64"',
                 '"S1" : "val16",\n"i2" : 16,\n"S3" : "val64",\n"i4" : 64'),
                ('"S3" : "val8"',
                 '"S1" : "val4",\n"i2" : 4,\n"S3" : "val8",\n"i4" : 8')))
        self.check_json(uri4index3, (
                ('"i2" : 1,\n"i4" : 1',
                 '"S1" : "val1",\n"i2" : 1,\n"S3" : "val1",\n"i4" : 1'),
                ('"i2" : 4,\n"i4" : 8',
                 '"S1" : "val4",\n"i2" : 4,\n"S3" : "val8",\n"i4" : 8'),
                ('"i2" : 9,\n"i4" : 27',
                 '"S1" : "val9",\n"i2" : 9,\n"S3" : "val27",\n"i4" : 27'),
                ('"i2" : 16,\n"i4" : 64',
                 '"S1" : "val16",\n"i2" : 16,\n"S3" : "val64",\n"i4" : 64')))

if __name__ == '__main__':
    wttest.run()
