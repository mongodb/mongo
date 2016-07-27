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
from suite_subprocess import suite_subprocess

# test_jsondump.py
# Test dump output from json cursors.
class test_jsondump02(wttest.WiredTigerTestCase, suite_subprocess):

    table_uri1 = 'table:jsondump02a.wt'
    table_uri2 = 'table:jsondump02b.wt'
    table_uri3 = 'table:jsondump02c.wt'
    basename_uri4 = 'jsondump02d.wt'
    table_uri4 = 'table:' + basename_uri4
    table_uri5 = 'table:jsondump02e.wt'
    table_uri6 = 'table:jsondump02f.wt'

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
                cursor[insert[0]] = insert[1]
        finally:
            cursor.close()

    def test_json_cursor(self):
        """
        Create JSON cursors and test them directly, also test
        dump/load commands.
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
        self.set_kv(self.table_uri1, 'KEY001', '\'\"({[]})\"\'\\, etc. allowed')
        # \u03c0 is pi in Unicode, converted by Python to UTF-8: 0xcf 0x80.
        # Here's how UTF-8 might be used.
        self.set_kv(self.table_uri1, 'KEY002', u'\u03c0'.encode('utf-8'))
        # 0xf5-0xff are illegal in Unicode, but may occur legally in C strings.
        self.set_kv(self.table_uri1, 'KEY003', '\xff\xfe')
        self.set_kv2(self.table_uri2, 'KEY000', 123, 'str0')
        self.set_kv2(self.table_uri2, 'KEY001', 234, 'str1')
        self.set_kv(self.table_uri3, 1, '\x01\x02\x03')
        self.set_kv(self.table_uri3, 2, '\x77\x88\x99\x00\xff\xfe')
        self.populate_squarecube(self.table_uri4)

        table1_json =  (
            ('"key0" : "KEY000"', '"value0" : "string value"'),
            ('"key0" : "KEY001"', '"value0" : ' +
             '"\'\\\"({[]})\\\"\'\\\\, etc. allowed"'),
            ('"key0" : "KEY002"', '"value0" : "\\u00cf\\u0080"'),
            ('"key0" : "KEY003"', '"value0" : "\\u00ff\\u00fe"'))
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
              (('"key0" : "KEY002"', '"value0" : 34,\n"value1" : "str2"'),))

        # extraneous/missing space is okay
        self.load_json(self.table_uri2,
              (('  "key0"\n:\t"KEY003"    ',
                '"value0":45,"value1"\n\n\r\n:\t\n"str3"'),))

        table2_json =  (
            ('"key0" : "KEY002"', '"value0" : 34,\n"value1" : "str2"'),
            ('"key0" : "KEY003"', '"value0" : 45,\n"value1" : "str3"'))

        table3_json =  (
            ('"key0" : 1', '"value0" : "\\u0001\\u0002\\u0003"'),
            ('"key0" : 2',
             '"value0" : "\\u0077\\u0088\\u0099\\u0000\\u00ff\\u00fe"'))
        self.check_json(self.table_uri3, table3_json)
        table4_json = (
                ('"ikey" : 1,\n"Skey" : "key1"',
                 '"S1" : "val1",\n"i2" : 1,\n"S3" : "val1",\n"i4" : 1'),
                ('"ikey" : 2,\n"Skey" : "key2"',
                 '"S1" : "val4",\n"i2" : 4,\n"S3" : "val8",\n"i4" : 8'),
                ('"ikey" : 3,\n"Skey" : "key3"',
                 '"S1" : "val9",\n"i2" : 9,\n"S3" : "val27",\n"i4" : 27'),
                ('"ikey" : 4,\n"Skey" : "key4"',
                 '"S1" : "val16",\n"i2" : 16,\n"S3" : "val64",\n"i4" : 64'))
        self.check_json(self.table_uri4, table4_json)
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

        # Dump all the tables into a single file, and also each
        # table into its own file.
        self.runWt(['dump', '-j',
                    self.table_uri1,
                    self.table_uri2,
                    self.table_uri3,
                    self.table_uri4],
                   outfilename='jsondump-all.out')
        self.runWt(['dump', '-j', self.table_uri1], outfilename='jsondump1.out')
        self.runWt(['dump', '-j', self.table_uri2], outfilename='jsondump2.out')
        self.runWt(['dump', '-j', self.table_uri3], outfilename='jsondump3.out')
        self.runWt(['dump', '-j', self.table_uri4], outfilename='jsondump4.out')
        self.session.drop(self.table_uri1)
        self.session.drop(self.table_uri2)
        self.session.drop(self.table_uri3)
        self.session.drop(self.table_uri4)
        self.runWt(['load', '-jf', 'jsondump1.out'])
        self.session.drop(self.table_uri1)
        self.runWt(['load', '-jf', 'jsondump2.out'])
        self.session.drop(self.table_uri2)
        self.runWt(['load', '-jf', 'jsondump3.out'])
        self.session.drop(self.table_uri3)
        self.runWt(['load', '-jf', 'jsondump4.out'])
        self.session.drop(self.table_uri4)

        self.runWt(['load', '-jf', 'jsondump-all.out'])
        self.check_json(self.table_uri1, table1_json)
        self.check_json(self.table_uri2, table2_json)
        self.check_json(self.table_uri3, table3_json)
        self.check_json(self.table_uri4, table4_json)

    # Generate two byte keys that cover some range of byte values.
    # For simplicity, the keys are monotonically increasing.
    # A null byte is disallowed in a string key, so we don't use it.
    def generate_key(self, i, k):
        k[0] = ((i & 0xffc0) >> 6) + 1
        k[1] = (i & 0x3f) + 1

    # Generate three byte values:
    #    i==0  :  v:[0x00, 0x01, 0x02]
    #    i==1  :  v:[0x01, 0x02, 0x03]
    # etc.
    # A null byte is disallowed in a string value, it is replaced by 'X'
    def generate_value(self, i, v, isstring):
        for j in range(0, 3):
            val = (i + j) % 256
            if isstring and val == 0:
                val = 88  # 'X'
            v[j] = val

    def test_json_all_bytes(self):
        """
        Test the generated JSON for all byte values in byte array and
        string formats.
        """
        self.session.create(self.table_uri5, 'key_format=u,value_format=u')
        self.session.create(self.table_uri6, 'key_format=S,value_format=S')

        c5 = self.session.open_cursor(self.table_uri5, None, None)
        c6 = self.session.open_cursor(self.table_uri6, None, None)
        k = bytearray(b'\x00\x00')
        v = bytearray(b'\x00\x00\x00')
        for i in range(0, 512):
            self.generate_key(i, k)
            self.generate_value(i, v, False)
            c5[str(k)] = str(v)
            self.generate_value(i, v, True)   # no embedded nuls
            c6[str(k)] = str(v)
        c5.close()
        c6.close()

        # Build table5_json, we want it to look like this:
        #    ('"key0" : "\u0001\u0001"', '"value0" : "\u0000\u0001\u0002"'),
        #    ('"key0" : "\u0001\u0002"', '"value0" : "\u0001\u0002\u0003"'))
        #    ('"key0" : "\u0001\u0003"', '"value0" : "\u0003\u0003\u0004"'))
        #    ...
        # table6_json is similar, except that printable values like '\u0041'
        # would appear as 'A'.  The string type cannot have embedded nulls,
        # so '\u0000' in table6_json appears instead as an 'X'.
        #
        # Start by creating two tables of individual Unicode values.
        # bin_unicode[] contains only the \u escape sequences.
        # mix_unicode[] contains printable characters or \t \n etc. escapes
        bin_unicode = []
        mix_unicode = []
        for i in range(0, 256):
            u = "\\u00" + hex(256 + i)[3:]  # e.g. "\u00ab")
            bin_unicode.append(u)
            mix_unicode.append(u)
        for i in range(0x20, 0x7f):
            mix_unicode[i] = chr(i)
        mix_unicode[ord('"')] = '\\"'
        mix_unicode[ord('\\')] = '\\\\'
        mix_unicode[ord('\f')] = '\\f'
        mix_unicode[ord('\n')] = '\\n'
        mix_unicode[ord('\r')] = '\\r'
        mix_unicode[ord('\t')] = '\\t'

        table5_json = []
        table6_json = []
        for i in range(0, 512):
            self.generate_key(i, k)
            self.generate_value(i, v, False)
            j = i if (i > 0 and i < 254) or (i > 256 and i < 510) else 88
            table5_json.append(('"key0" : "' + bin_unicode[k[0]] +
                                bin_unicode[k[1]] + '"',
                                '"value0" : "' + bin_unicode[v[0]] +
                                bin_unicode[v[1]] +
                                bin_unicode[v[2]] + '"'))
            self.generate_value(i, v, True)
            table6_json.append(('"key0" : "' + mix_unicode[k[0]] +
                                mix_unicode[k[1]] + '"',
                                '"value0" : "' + mix_unicode[v[0]] +
                                mix_unicode[v[1]] +
                                mix_unicode[v[2]] + '"'))

        self.check_json(self.table_uri5, table5_json)
        self.check_json(self.table_uri6, table6_json)

        self.session.truncate(self.table_uri5, None, None, None)
        self.session.truncate(self.table_uri6, None, None, None)
        self.load_json(self.table_uri5, table5_json)
        self.load_json(self.table_uri6, table6_json)
        self.check_json(self.table_uri5, table5_json)
        self.check_json(self.table_uri6, table6_json)

        self.runWt(['dump', '-j', self.table_uri5], outfilename='jsondump5.out')
        self.runWt(['dump', '-j', self.table_uri6], outfilename='jsondump6.out')
        self.session.drop(self.table_uri5)
        self.session.drop(self.table_uri6)
        self.runWt(['load', '-jf', 'jsondump5.out'])
        self.runWt(['load', '-jf', 'jsondump6.out'])
        self.session.drop(self.table_uri5)
        self.session.drop(self.table_uri6)


if __name__ == '__main__':
    wttest.run()
