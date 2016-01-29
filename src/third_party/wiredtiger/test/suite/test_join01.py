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
from wtscenario import check_scenarios, multiply_scenarios, number_scenarios

# test_join01.py
#    Join operations
# Basic tests for join
class test_join01(wttest.WiredTigerTestCase):
    table_name1 = 'test_join01'
    nentries = 100

    scenarios = [
        ('table', dict(ref='table')),
        ('index', dict(ref='index'))
    ]
    # We need statistics for these tests.
    conn_config = 'statistics=(all)'

    def gen_key(self, i):
        return [ i + 1 ]

    def gen_values(self, i):
        s = str(i)
        rs = s[::-1]
        sort3 = (self.nentries * (i % 3)) + i    # multiples of 3 sort first
        return [s, rs, sort3]

    # Common function for testing iteration of join cursors
    def iter_common(self, jc, do_proj):
        # See comments in join_common()
        expect = [73, 82, 62, 83, 92]
        while jc.next() == 0:
            [k] = jc.get_keys()
            i = k - 1
            if do_proj:  # our projection test simply reverses the values
                [v2,v1,v0] = jc.get_values()
            else:
                [v0,v1,v2] = jc.get_values()
            self.assertEquals(self.gen_values(i), [v0,v1,v2])
            if len(expect) == 0 or i != expect[0]:
                self.tty('  result ' + str(i) + ' is not in: ' + str(expect))
            self.assertTrue(i == expect[0])
            expect.remove(i)
        self.assertEquals(0, len(expect))

    # Stats are collected twice: after iterating
    # through the join cursor once, and secondly after resetting
    # the join cursor and iterating again.
    def stats(self, jc, which):
        statcur = self.session.open_cursor('statistics:join', jc, None)
        self.check_stats(statcur, 0, 'join: index:join01:index1: ' +
                         'bloom filter false positives')
        statcur.close()

    def statstr_to_int(self, str):
        """
        Convert a statistics value string, which may be in either form:
        '12345' or '33M (33604836)'
        """
        parts = str.rpartition('(')
        return int(parts[2].rstrip(')'))

    # string should appear with a minimum value of least "min".
    def check_stats(self, statcursor, min, lookfor):
        stringclass = ''.__class__
        intclass = (0).__class__

        # Reset the cursor, we're called multiple times.
        statcursor.reset()

        found = False
        foundval = 0
        self.printVerbose(3, 'statistics:')
        for id, desc, valstr, val in statcursor:
            self.assertEqual(type(desc), stringclass)
            self.assertEqual(type(valstr), stringclass)
            self.assertEqual(type(val), intclass)
            self.assertEqual(val, self.statstr_to_int(valstr))
            self.printVerbose(3, '  stat: \'' + desc + '\', \'' +
                              valstr + '\', ' + str(val))
            if desc == lookfor:
                found = True
                foundval = val

        self.assertTrue(found, 'in stats, did not see: ' + lookfor)
        self.assertTrue(foundval >= min)

    # Common function for testing the most basic functionality
    # of joins
    def join_common(self, joincfg0, joincfg1, do_proj, do_stats):
        #self.tty('join_common(' + joincfg0 + ',' + joincfg1 + ',' +
        #         str(do_proj) + ')')
        self.session.create('table:join01', 'key_format=r' +
                            ',value_format=SSi,columns=(k,v0,v1,v2)')
        self.session.create('index:join01:index0','columns=(v0)')
        self.session.create('index:join01:index1','columns=(v1)')
        self.session.create('index:join01:index2','columns=(v2)')

        c = self.session.open_cursor('table:join01', None, None)
        for i in range(0, self.nentries):
            c.set_key(*self.gen_key(i))
            c.set_value(*self.gen_values(i))
            c.insert()
        c.close()

        if do_proj:
            proj_suffix = '(v2,v1,v0)'  # Reversed values
        else:
            proj_suffix = ''            # Default projection (v0,v1,v2)

        # We join on index2 first, not using bloom indices.
        # This defines the order that items are returned.
        # index2 is sorts multiples of 3 first (see gen_values())
        # and by using 'gt' and key 99, we'll skip multiples of 3,
        # and examine primary keys 2,5,8,...,95,98,1,4,7,...,94,97.
        jc = self.session.open_cursor('join:table:join01' + proj_suffix,
                                      None, None)
        c2 = self.session.open_cursor('index:join01:index2', None, None)
        c2.set_key(99)   # skips all entries w/ primary key divisible by three
        self.assertEquals(0, c2.search())
        self.session.join(jc, c2, 'compare=gt')

        # Then select all the numbers 0-99 whose string representation
        # sort >= '60'.
        if self.ref == 'index':
            c0 = self.session.open_cursor('index:join01:index0', None, None)
            c0.set_key('60')
        else:
            c0 = self.session.open_cursor('table:join01', None, None)
            c0.set_key(60)
        self.assertEquals(0, c0.search())
        self.session.join(jc, c0, 'compare=ge' + joincfg0)

        # Then select all numbers whose reverse string representation
        # is in '20' < x < '40'.
        c1a = self.session.open_cursor('index:join01:index1', None, None)
        c1a.set_key('21')
        self.assertEquals(0, c1a.search())
        self.session.join(jc, c1a, 'compare=gt' + joincfg1)

        c1b = self.session.open_cursor('index:join01:index1', None, None)
        c1b.set_key('41')
        self.assertEquals(0, c1b.search())
        self.session.join(jc, c1b, 'compare=lt' + joincfg1)

        # Numbers that satisfy these 3 conditions (with ordering implied by c2):
        #    [73, 82, 62, 83, 92].
        #
        # After iterating, we should be able to reset and iterate again.
        if do_stats:
            self.stats(jc, 0)
        self.iter_common(jc, do_proj)
        if do_stats:
            self.stats(jc, 1)
        jc.reset()
        self.iter_common(jc, do_proj)
        if do_stats:
            self.stats(jc, 2)
        jc.reset()
        self.iter_common(jc, do_proj)

        jc.close()
        c2.close()
        c1a.close()
        c1b.close()
        c0.close()
        self.session.drop('table:join01')

    # Test joins with basic functionality
    def test_join(self):
        bloomcfg1000 = ',strategy=bloom,count=1000'
        bloomcfg10000 = ',strategy=bloom,count=10000'
        for cfga in [ '', bloomcfg1000, bloomcfg10000 ]:
            for cfgb in [ '', bloomcfg1000, bloomcfg10000 ]:
                for do_proj in [ False, True ]:
                    #self.tty('cfga=' + cfga +
                    #         ', cfgb=' + cfgb +
                    #         ', doproj=' + str(do_proj))
                    self.join_common(cfga, cfgb, do_proj, False)

    def test_join_errors(self):
        self.session.create('table:join01', 'key_format=r,value_format=SS'
                            ',columns=(k,v0,v1)')
        self.session.create('table:join01B', 'key_format=r,value_format=SS'
                            ',columns=(k,v0,v1)')
        self.session.create('index:join01:index0','columns=(v0)')
        self.session.create('index:join01:index1','columns=(v1)')
        self.session.create('index:join01B:index0','columns=(v0)')
        jc = self.session.open_cursor('join:table:join01', None, None)
        tc = self.session.open_cursor('table:join01', None, None)
        fc = self.session.open_cursor('file:join01.wt', None, None)
        ic0 = self.session.open_cursor('index:join01:index0', None, None)
        ic0again = self.session.open_cursor('index:join01:index0', None, None)
        ic1 = self.session.open_cursor('index:join01:index1', None, None)
        icB = self.session.open_cursor('index:join01B:index0', None, None)
        tcB = self.session.open_cursor('table:join01B', None, None)

        tc.set_key(1)
        tc.set_value('val1', 'val1')
        tc.insert()
        tcB.set_key(1)
        tcB.set_value('val1', 'val1')
        tcB.insert()
        fc.next()

        # Joining using a non join-cursor
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.join(tc, ic0, 'compare=ge'),
            '/not a join cursor/')
        # Joining a table cursor, not index
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.join(jc, fc, 'compare=ge'),
            '/not an index or table cursor/')
        # Joining a non positioned cursor
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.join(jc, ic0, 'compare=ge'),
            '/requires reference cursor be positioned/')
        ic0.set_key('val1')
        # Joining a non positioned cursor (no search or next has been done)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.join(jc, ic0, 'compare=ge'),
            '/requires reference cursor be positioned/')
        ic0.set_key('valXX')
        self.assertEqual(ic0.search(), wiredtiger.WT_NOTFOUND)
        # Joining a non positioned cursor after failed search
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.join(jc, ic0, 'compare=ge'),
            '/requires reference cursor be positioned/')

        # position the cursors now
        ic0.set_key('val1')
        ic0.search()
        ic0again.next()
        icB.next()

        # Joining non matching index
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.join(jc, icB, 'compare=ge'),
            '/table for join cursor does not match/')

        # The cursor must be positioned
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.join(jc, ic1, 'compare=ge'),
            '/requires reference cursor be positioned/')
        ic1.next()

        # The first cursor joined cannot be bloom
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.join(jc, ic1,
            'compare=ge,strategy=bloom,count=1000'),
            '/first joined cursor cannot specify strategy=bloom/')

        # This succeeds.
        self.session.join(jc, ic1, 'compare=ge'),

        # With bloom filters, a count is required
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.join(jc, ic0, 'compare=ge,strategy=bloom'),
            '/count must be nonzero/')

        # This succeeds.
        self.session.join(jc, ic0, 'compare=ge,strategy=bloom,count=1000'),

        bloom_config = ',strategy=bloom,count=1000'
        # Cannot use the same index cursor
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.join(jc, ic0,
            'compare=le' + bloom_config),
            '/index cursor already used in a join/')

        # When joining with the same index, need compatible compares
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.join(jc, ic0again, 'compare=ge' + bloom_config),
            '/join has overlapping ranges/')

        # Another incompatible compare
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.join(jc, ic0again, 'compare=gt' + bloom_config),
            '/join has overlapping ranges/')

        # Compare is compatible, but bloom args need to match
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.join(jc, ic0again, 'compare=le'),
            '/join has incompatible strategy/')

        # Counts need to match for bloom filters
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.join(jc, ic0again, 'compare=le,strategy=bloom,'
            'count=100'), '/count.* does not match previous count/')

        # This succeeds
        self.session.join(jc, ic0again, 'compare=le,strategy=bloom,count=1000')

        # Need to do initial next() before getting key/values
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: jc.get_keys(),
            '/join cursor must be advanced with next/')

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: jc.get_values(),
            '/join cursor must be advanced with next/')

        # Operations on the joined cursor are frozen until the join is closed.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: ic0.next(),
            '/index cursor is being used in a join/')

        # Operations on the joined cursor are frozen until the join is closed.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: ic0.prev(),
            '/index cursor is being used in a join/')

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: ic0.reset(),
            '/index cursor is being used in a join/')

        # Only a small number of operations allowed on a join cursor
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: jc.search())

        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: jc.prev())

        self.assertEquals(jc.next(), 0)
        self.assertEquals(jc.next(), wiredtiger.WT_NOTFOUND)

        # Only after the join cursor is closed can we use the index cursor
        # normally
        jc.close()
        self.assertEquals(ic0.next(), wiredtiger.WT_NOTFOUND)
        self.assertEquals(ic0.prev(), 0)

    # common code for making sure that cursors can be
    # implicitly closed, no matter the order they are created
    def cursor_close_common(self, joinfirst):
        self.session.create('table:join01', 'key_format=r' +
                            ',value_format=SS,columns=(k,v0,v1)')
        self.session.create('index:join01:index0','columns=(v0)')
        self.session.create('index:join01:index1','columns=(v1)')
        c = self.session.open_cursor('table:join01', None, None)
        for i in range(0, self.nentries):
            c.set_key(*self.gen_key(i))
            c.set_value(*self.gen_values(i))
            c.insert()
        c.close()

        if joinfirst:
            jc = self.session.open_cursor('join:table:join01', None, None)
        c0 = self.session.open_cursor('index:join01:index0', None, None)
        c1 = self.session.open_cursor('index:join01:index1', None, None)
        c0.next()        # index cursors must be positioned
        c1.next()
        if not joinfirst:
            jc = self.session.open_cursor('join:table:join01', None, None)
        self.session.join(jc, c0, 'compare=ge')
        self.session.join(jc, c1, 'compare=ge')
        self.session.close()
        self.session = None

    def test_cursor_close1(self):
        self.cursor_close_common(True)

    def test_cursor_close2(self):
        self.cursor_close_common(False)

    def test_stats(self):
        bloomcfg1000 = ',strategy=bloom,count=1000'
        bloomcfg10 = ',strategy=bloom,count=10'
        self.join_common(bloomcfg1000, bloomcfg1000, False, True)

        # Intentially run with an underconfigured Bloom filter,
        # statistics should pick up some false positives.
        self.join_common(bloomcfg10, bloomcfg10, False, True)


if __name__ == '__main__':
    wttest.run()
