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

import wiredtiger, wttest, suite_random
from wtscenario import check_scenarios, multiply_scenarios, number_scenarios

# test_join02.py
#    Join operations
# Join several indices together, trying all comparison combinations
class test_join02(wttest.WiredTigerTestCase):
    table_name1 = 'test_join02'
    nentries = 1000

    keyscen = [
        ('key-r', dict(keyformat='r')),
        ('key-S', dict(keyformat='S')),
        ('key-i', dict(keyformat='i')),
        ('key-iS', dict(keyformat='iS'))
    ]

    bloomscen = [
        ('bloom', dict(usebloom=True)),
        ('nobloom', dict(usebloom=False))
    ]

    scenarios = number_scenarios(multiply_scenarios('.', keyscen, bloomscen))

    # Start our range from 1, since WT record numbers start at 1,
    # it makes things work out nicer.
    def range(self):
        return range(1, self.nentries + 1)

    def gen_key(self, i):
        if self.keyformat == 'S':
            return [ 'key%06d' % i ]  # zero pad so it sorts expectedly
        elif self.keyformat == 'iS':
            return [ i, 'key%06d' % i ]
        else:
            return [ i ]

    def gen_values(self, i):
        s = str(i)
        x = 'x' * i
        rs = s[::-1]
        f = int(s[0:1])
        return [i, s, x, rs, f]

    def reinit_joinconfig(self):
        self.rand = suite_random.suite_random(self.seed)
        self.seed += 1

    def get_joinconfig(self):
        # When we're running the bloom scenario, make it so the
        # bloom filters are often shared. Make the number of
        # hashes and number of bits per item so they don't always
        # match up; WT should allow it.
        if self.usebloom:
            c = 10000 if (self.rand.rand32() % 3) != 0 else 100000
            k = 8 if (self.rand.rand32() % 10) != 0 else 10
            b = 16 if (self.rand.rand32() % 11) != 0 else 12
            return \
                ',strategy=bloom,count=' + str(c) + \
                ',bloom_bit_count=' + str(b) + \
                ',bloom_hash_count=' + str(k)
        else:
            return ''

    def do_join(self, jc, curleft, curright, choice, mbr):
        c0 = choice[0]
        if c0 == None:
            return mbr
        # The first join cannot use a bloom filter
        if jc.first_join:
            joinconfig = ''
            jc.first_join = False
        else:
            joinconfig = self.get_joinconfig()
        if c0 != None:
            #self.tty('join(jc, ' + curleft.name + ' ' + c0 +
            #         ' ' + str(curleft.low) + ')')
            curleft.reset()
            curleft.set_key(*curleft.low)
            self.assertEquals(0, curleft.search())
            self.session.join(jc, curleft, 'compare=' + c0 + joinconfig)
            if c0 == 'eq':
                mbr = mbr.intersection(curleft.eqmembers)
            elif c0 == 'ge':
                mbr = mbr.intersection(
                    set(curleft.eqmembers.union(curleft.gtmembers)))
            elif c0 == 'gt':
                mbr = mbr.intersection(curleft.gtmembers)
        c1 = choice[1] if len(choice) > 1 else None
        if c1 != None:
            #self.tty('join(jc, ' + curright.name + ' ' + c1 +
            #         ' ' + str(curright.high) + ')')
            curright.reset()
            curright.set_key(*curright.high)
            self.assertEquals(0, curright.search())
            self.session.join(jc, curright, 'compare=' + c1 + joinconfig)
            if c1 == 'le':
                mbr = mbr.intersection(
                    set(curright.eqmembers.union(curright.ltmembers)))
            elif c1 == 'lt':
                mbr = mbr.intersection(curright.ltmembers)
        return mbr

    def iterate(self, jc, mbr):
        #self.tty('iteration expects ' + str(len(mbr)) +
        #         ' entries: ' + str(mbr))
        while jc.next() == 0:
            keys = jc.get_keys()
            [v0,v1,v2,v3,v4] = jc.get_values()
            k0 = keys[0]
            k1 = keys[1] if len(keys) > 1 else None
            if self.keyformat == 'S':
                i = int(str(k0[3:]))
            elif self.keyformat == 'iS':
                i = k0
                self.assertEquals(i, int(str(k1[3:])))
            else:
                i = k0
            #self.tty('  iteration got key: ' + str(k0) + ',' + str(k1))
            #self.tty('  iteration got values: ' + str([v0,v1,v2,v3,v4]))
            #self.tty('  iteration expects values: ' + str(self.gen_values(i)))
            self.assertEquals(self.gen_values(i), [v0,v1,v2,v3,v4])
            if not i in mbr:
                self.tty('  result ' + str(i) + ' is not in: ' + str(mbr))
            self.assertTrue(i in mbr)
            mbr.remove(i)
        self.assertEquals(0, len(mbr))

    def mkmbr(self, expr):
        return frozenset([x for x in self.range() if expr(x)])

    def test_basic_join(self):
        self.seed = 1
        if self.keyformat == 'iS':
            keycols = 'k0,k1'
        else:
            keycols = 'k'
        self.session.create('table:join02', 'key_format=' + self.keyformat +
                            ',value_format=iSuSi,columns=(' + keycols +
                            ',v0,v1,v2,v3,v4)')
        self.session.create('index:join02:index0','columns=(v0)')
        self.session.create('index:join02:index1','columns=(v1)')
        self.session.create('index:join02:index2','columns=(v2)')
        self.session.create('index:join02:index3','columns=(v3)')
        self.session.create('index:join02:index4','columns=(v4)')
        c = self.session.open_cursor('table:join02', None, None)
        for i in self.range():
            c.set_key(*self.gen_key(i))
            c.set_value(*self.gen_values(i))
            c.insert()
        c.close()

        # Use the primary table in one of the joins.
        # Use various projections, which should not matter for ref cursors
        c0a = self.session.open_cursor('table:join02', None, None)
        c0b = self.session.open_cursor('table:join02(v4)', None, None)
        c1a = self.session.open_cursor('index:join02:index1(v0)', None, None)
        c1b = self.session.open_cursor('index:join02:index1', None, None)
        c2a = self.session.open_cursor('index:join02:index2', None, None)
        c2b = self.session.open_cursor('index:join02:index2', None, None)
        c3a = self.session.open_cursor('index:join02:index3(v4)', None, None)
        c3b = self.session.open_cursor('index:join02:index3(v0)', None, None)
        c4a = self.session.open_cursor('index:join02:index4(v1)', None, None)

        # Attach extra properties to each cursor.  For cursors that
        # may appear on the 'left' side of a range CA < x < CB,
        # we give a low value of the range, and calculate the members
        # of the set we expect to see for a 'gt' comparison, as well
        # as the 'eq' comparison.  For cursors that appear on the
        # 'right side of the range, we give a high value of the range,
        # and calculate membership sets for 'lt' and 'eq'.
        #
        # We've defined the low/high values so that there's a lot of
        # overlap between the values when we're doing ranges.
        c0a.name = 'c0a'
        c0b.name = 'c0b'
        if self.keyformat == 'i' or self.keyformat == 'r':
            c0a.low = [ 205 ]
            c0b.high = [ 990 ]
        elif self.keyformat == 'S':
            c0a.low = [ 'key000205' ]
            c0b.high = [ 'key000990' ]
        elif self.keyformat == 'iS':
            c0a.low = [ 205, 'key000205' ]
            c0b.high = [ 990, 'key000990' ]
        c0a.gtmembers = self.mkmbr(lambda x: x > 205)
        c0a.eqmembers = self.mkmbr(lambda x: x == 205)
        c0b.ltmembers = self.mkmbr(lambda x: x < 990)
        c0b.eqmembers = self.mkmbr(lambda x: x == 990)

        c1a.low = [ '150' ]
        c1a.gtmembers = self.mkmbr(lambda x: str(x) > '150')
        c1a.eqmembers = self.mkmbr(lambda x: str(x) == '150')
        c1a.name = 'c1a'
        c1b.high = [ '733' ]
        c1b.ltmembers = self.mkmbr(lambda x: str(x) < '733')
        c1b.eqmembers = self.mkmbr(lambda x: str(x) == '733')
        c1b.name = 'c1b'

        c2a.low = [ 'x' * 321 ]
        c2a.gtmembers = self.mkmbr(lambda x: x > 321)
        c2a.eqmembers = self.mkmbr(lambda x: x == 321)
        c2a.name = 'c2a'
        c2b.high = [ 'x' * 765 ]
        c2b.ltmembers = self.mkmbr(lambda x: x < 765)
        c2b.eqmembers = self.mkmbr(lambda x: x == 765)
        c2b.name = 'c2b'

        c3a.low = [ '432' ]
        c3a.gtmembers = self.mkmbr(lambda x: str(x)[::-1] > '432')
        c3a.eqmembers = self.mkmbr(lambda x: str(x)[::-1] == '432')
        c3a.name = 'c3a'
        c3b.high = [ '876' ]
        c3b.ltmembers = self.mkmbr(lambda x: str(x)[::-1] < '876')
        c3b.eqmembers = self.mkmbr(lambda x: str(x)[::-1] == '876')
        c3b.name = 'c3b'

        c4a.low = [ 4 ]
        c4a.gtmembers = self.mkmbr(lambda x: str(x)[0:1] > '4')
        c4a.eqmembers = self.mkmbr(lambda x: str(x)[0:1] == '4')
        c4a.name = 'c4a'

        choices = [[None], ['eq'], ['ge'], ['gt'], [None, 'le'], [None, 'lt'],
                   ['ge', 'le' ], ['ge', 'lt' ], ['gt', 'le' ], ['gt', 'lt' ]]
        smallchoices = [[None], ['eq'], ['ge'], ['gt', 'le' ]]
        for i0 in smallchoices:
            for i1 in choices:
                for i2 in smallchoices:
                    for i3 in smallchoices:
                        for i4 in [[None], ['eq'], ['ge'], ['gt']]:
                            if i0[0] == None and i1[0] == None and \
                               i2[0] == None and i3[0] == None and \
                               i4[0] == None:
                                continue
                            self.reinit_joinconfig()
                            #self.tty('Begin test: ' +
                            #         ','.join([str(i0),str(i1),str(i2),
                            #                   str(i3),str(i4)]))
                            jc = self.session.open_cursor('join:table:join02',
                                                          None, None)
                            jc.first_join = True
                            mbr = set(self.range())

                            # It shouldn't matter the order of the joins
                            mbr = self.do_join(jc, c3a, c3b, i3, mbr)
                            mbr = self.do_join(jc, c2a, c2b, i2, mbr)
                            mbr = self.do_join(jc, c4a, None, i4, mbr)
                            mbr = self.do_join(jc, c1a, c1b, i1, mbr)
                            mbr = self.do_join(jc, c0a, c0b, i0, mbr)
                            self.iterate(jc, mbr)
                            jc.close()
        c0a.close()
        c0b.close()
        c1a.close()
        c1b.close()
        c2a.close()
        c2b.close()
        c3a.close()
        c3b.close()
        c4a.close()

if __name__ == '__main__':
    wttest.run()
