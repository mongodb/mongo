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

from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios
import wttest
from wiredtiger import stat

# test_stat04.py
#    Statistics key/value pair count
class test_stat04(wttest.WiredTigerTestCase, suite_subprocess):
    uripfx = 'table:test_stat04.'

    keyfmt = [
        ('col', dict(keyfmt='r', valuefmt='S', storekind='col')),
        ('fix', dict(keyfmt='r', valuefmt='8t', storekind='fix')),
        ('row', dict(keyfmt='S', valuefmt='S', storekind='row')),
    ]
    nentries = [
        ('small', dict(nentries=100, valuesize=50)),
        ('medium', dict(nentries=10000, valuesize=20)),
        ('large', dict(nentries=100000, valuesize=1)),
        ('jumboval', dict(nentries=100, valuesize=4200000)),
    ]
    scenarios = make_scenarios(keyfmt, nentries)
    conn_config = 'statistics=(all)'

    def init_test(self):
        self.valuepfx = self.valuesize * 'X'

    def genkey(self, n):
        if self.keyfmt == 'S':
            return 'SOMEKEY' + str(n)
        else:
            return n + 1

    def genvalue(self, n):
        if self.valuefmt == 'S':
            return self.valuepfx + str(n)
        else:
            return n & 0xff

    def checkcount(self, uri, expectpairs):
        statcursor = self.session.open_cursor(
            'statistics:' + uri, None, 'statistics=(all,clear)')
        self.assertEqual(statcursor[stat.dsrc.btree_entries][2], expectpairs)
        statcursor.close()

    def test_stat_nentries(self):
        """
        Test to make sure the number of key/value pairs is accurate.
        """

        self.init_test()
        uri = self.uripfx + self.storekind + '.' + str(self.nentries)
        self.session.create(uri, 'key_format=' + self.keyfmt +
                            ',value_format=' + self.valuefmt)
        cursor = self.session.open_cursor(uri, None, None)

        count = 0
        # Insert entries, periodically checking that stats match.
        for i in range(0, self.nentries):
            if count % 50 == 0:
                self.checkcount(uri, count)
            cursor[self.genkey(i)] = self.genvalue(i)
            count += 1

        # Remove a number of entries, at each step checking that stats match.
        for i in range(0, self.nentries // 37):
            cursor.set_key(self.genkey(i*11 % self.nentries))
            if cursor.remove() == 0 and self.valuefmt != '8t':
                count -= 1
            self.checkcount(uri, count)
        cursor.close()

        # Confirm the count is correct after writing to the backing file,
        # that tests the on-disk format as well as the in-memory format.
        self.reopen_conn()
        self.checkcount(uri, count)

if __name__ == '__main__':
    wttest.run()
