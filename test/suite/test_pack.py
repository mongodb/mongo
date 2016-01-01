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
#
# test_pack.py
#    Tests packing using public methods
#

import wiredtiger, wttest
import re, sys

class test_pack(wttest.WiredTigerTestCase):
    name = 'test_pack'

    def dump_cursor(self, cursor, name):
        cursor.reset()
        while cursor.next() == 0:
            x = cursor.get_key()
            y = cursor.get_value()
            self.tty(' ' + name + ':  ' + str(x) + ' => ' + str(y))
        cursor.reset()

    def check(self, fmt, *v):
        v = list(v)
        fmtname = re.sub('([A-Z])', r'_\1', fmt)
        uri = 'table:' + test_pack.name + '-' + fmtname
        idx_uri = 'index:' + test_pack.name + '-' + fmtname + ':inverse'
        nargs = len(v)
        colnames = ",".join("v" + str(x) for x in xrange(nargs))
        self.session.create(uri, "columns=(k," + colnames + ")," +
                            "key_format=i,value_format=" + fmt)
        self.session.create(idx_uri, "columns=(" + colnames + ")")
        forw = self.session.open_cursor(uri, None, None)
        forw_idx = self.session.open_cursor(idx_uri + "(k)", None, None)

        forw.set_key(1234)
        forw.set_value(*v)
        forw.insert()

        #self.dump_cursor(forw, 'forw')
        #self.dump_cursor(forw_idx, 'index')

        forw.set_key(1234)
        self.assertEquals(forw.search(), 0)
        got = forw.get_value()
        if nargs == 1:  # API does not return a list, we want one for comparing
            got = [got]
        self.assertEquals(got, v)

        forw_idx.set_key(*v)
        self.assertEquals(forw_idx.search(), 0)
        self.assertEquals(forw_idx.get_value(), 1234)

    def test_packing(self):
        self.check('iii', 0, 101, -99)
        self.check('3i', 0, 101, -99)
        self.check('iS', 42, "forty two")

        self.check('S', 'abc')
        self.check('9S', 'a' * 9)
        self.check('9SS', "forty two", "spam egg")
        self.check('42S', 'a' * 42)
        self.check('42SS', 'a' * 42, 'something')
        self.check('S42S', 'something', 'a' * 42)
        # nul terminated string with padding
        self.check('10SS', 'aaaaa\x00\x00\x00\x00\x00', 'something')
        self.check('S10S', 'something', 'aaaaa\x00\x00\x00\x00\x00')

        self.check('u', r"\x42" * 20)
        self.check('uu', r"\x42" * 10, r"\x42" * 10)
        self.check('3u', r"\x4")
        self.check('3uu', r"\x4", r"\x42" * 10)
        self.check('u3u', r"\x42" * 10, r"\x4")

        self.check('s', "4")
        self.check("1s", "4")
        self.check("2s", "42")

if __name__ == '__main__':
    wttest.run()
