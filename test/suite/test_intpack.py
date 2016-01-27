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
# test_intpack.py
#    Tests integer packing using public methods
#

import wiredtiger, wttest
from wtscenario import check_scenarios, number_scenarios

class PackTester:
    def __init__(self, formatcode, validlow, validhigh, equals):
        self.formatcode = formatcode
        self.validlow = validlow
        self.validhigh = validhigh
        self.recno = 1
        self.forw = None          #  r -> code
        self.forw_idx = None      #  code -> r
        self.back = None          #  code -> r
        self.back_idx = None      #  r -> code
        self.session = None
        self.equals = equals
        self.forw_uri = None
        self.back_uri = None
        self.forw_idx_uri = None
        self.back_idx_uri = None

    def initialize(self, session):
        self.session = session
        pfx = 'test_intpack_'
        x = self.formatcode
        if x.isupper():
            x = x + '_' # differentiate upper case from lower case in our naming
        tab = pfx + x + 'forw'
        forw_uri = 'table:' + tab
        forw_idx_uri = 'index:' + tab + ':inverse'
        tab = pfx + x + 'back'
        back_uri = 'table:' + tab
        back_idx_uri = 'index:' + tab + ':inverse'

        session.create(forw_uri, "columns=(k,v)," +
                       "key_format=i,value_format=" + self.formatcode)
        session.create(forw_idx_uri, "columns=(v)")
        session.create(back_uri, "columns=(k,v)," +
                       "key_format=" + self.formatcode + ",value_format=i")
        session.create(back_idx_uri, "columns=(v)")
        self.forw_uri = forw_uri
        self.back_uri = back_uri
        self.forw_idx_uri = forw_idx_uri
        self.back_idx_uri = back_idx_uri
        self.truncate()

    def closeall(self):
        if self.forw != None:
            self.forw.close()
            self.forw_idx.close()
            self.back.close()
            self.back_idx.close()
            self.forw = None
            self.forw_idx = None
            self.back = None
            self.back_idx = None

    def truncate(self):
        self.closeall()
        self.session.truncate(self.forw_uri, None, None, None)
        self.session.truncate(self.back_uri, None, None, None)
        self.forw = self.session.open_cursor(self.forw_uri, None, None)
        self.forw_idx = self.session.open_cursor(self.forw_idx_uri + "(k)",
                                                 None, None)

        self.back = self.session.open_cursor(self.back_uri, None, None)
        self.back_idx = self.session.open_cursor(self.back_idx_uri + "(k)",
                                                 None, None)

    def check_range(self, low, high):
        if low < self.validlow:
            low = self.validlow
        if high > self.validhigh:
            high = self.validhigh
        i = low
        forw = self.forw
        forw_idx = self.forw_idx
        back = self.back
        back_idx = self.back_idx
        while i <= high:
            forw[self.recno] = i
            back[i] = self.recno
            self.equals(forw[self.recno], i)
            self.equals(forw_idx[i], self.recno)
            self.equals(back[i], self.recno)
            self.equals(back_idx[self.recno], i)
            forw.reset()
            forw_idx.reset()
            back.reset()
            back_idx.reset()
            self.recno += 1
            i += 1

# Test integer packing with various formats
class test_intpack(wttest.WiredTigerTestCase):
    name = 'test_intpack'

    scenarios = check_scenarios([
        ('b', dict(formatcode='b', low=-128, high=127, nbits=8)),
        ('B', dict(formatcode='B', low=0, high=255, nbits=8)),
        ('8t', dict(formatcode='8t', low=0, high=255, nbits=8)),
        ('5t', dict(formatcode='5t', low=0, high=31, nbits=5)),
        ('h', dict(formatcode='h', low=-32768, high=32767, nbits=16)),
        ('H', dict(formatcode='H', low=0, high=65535, nbits=16)),
        ('i', dict(formatcode='i', low=-2147483648, high=2147483647, nbits=32)),
        ('I', dict(formatcode='I', low=0, high=4294967295, nbits=32)),
        ('l', dict(formatcode='l', low=-2147483648, high=2147483647, nbits=32)),
        ('L', dict(formatcode='L', low=0, high=4294967295, nbits=32)),
        ('q', dict(formatcode='q', low=-9223372036854775808,
                   high=9223372036854775807, nbits=64)),
        ('Q', dict(formatcode='Q', low=0, high=18446744073709551615, nbits=64)),
    ])
    scenarios = check_scenarios(number_scenarios(scenarios))

    def test_packing(self):
        pt = PackTester(self.formatcode, self.low, self.high, self.assertEquals)
        self.assertEquals(2 ** self.nbits, self.high - self.low + 1)
        pt.initialize(self.session)
        pt.check_range(-66000, 66000)
        if self.nbits >= 32:
            e32 = 2 ** 32
            pt.check_range(e32 - 1000, e32 + 1000)
            pt.check_range(-e32 - 1000, -e32 + 1000)
        if self.nbits >= 64:
            e64 = 2 ** 64
            pt.check_range(e64 - 1000, e64 + 1000)
            pt.check_range(-e64 - 1000, -e64 + 1000)
            pt.truncate()
            i = 8
            while i < 1 << 60:
                pt.check_range(-i - 1, -i + 1)
                pt.check_range(i - 1, i + 1)
                i <<= 1

if __name__ == '__main__':
    wttest.run()
