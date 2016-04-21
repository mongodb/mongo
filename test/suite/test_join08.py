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

# test_join08.py
#    Test join error paths
class test_join08(wttest.WiredTigerTestCase):
    nentries = 100

    # We need statistics for these tests.
    conn_config = 'statistics=(all)'

    def gen_key(self, i):
        return [ i + 1 ]

    def gen_values(self, i):
        s = str(i)
        rs = s[::-1]
        sort3 = (self.nentries * (i % 3)) + i    # multiples of 3 sort first
        return [s, rs, sort3]

    def test_join_errors(self):
        self.session.create('table:join08', 'key_format=r,value_format=SS'
                            ',columns=(k,v0,v1)')
        self.session.create('table:join08B', 'key_format=r,value_format=SS'
                            ',columns=(k,v0,v1)')
        self.session.create('index:join08:index0','columns=(v0)')
        self.session.create('index:join08:index1','columns=(v1)')
        self.session.create('index:join08B:index0','columns=(v0)')
        jc = self.session.open_cursor('join:table:join08', None, None)
        tc = self.session.open_cursor('table:join08', None, None)
        fc = self.session.open_cursor('file:join08.wt', None, None)
        ic0 = self.session.open_cursor('index:join08:index0', None, None)
        ic0again = self.session.open_cursor('index:join08:index0', None, None)
        ic1 = self.session.open_cursor('index:join08:index1', None, None)
        icB = self.session.open_cursor('index:join08B:index0', None, None)
        tcB = self.session.open_cursor('table:join08B', None, None)

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
            '/must be an index, table or join cursor/')
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
            '/cursor already used in a join/')

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
            '/cursor is being used in a join/')

        # Operations on the joined cursor are frozen until the join is closed.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: ic0.prev(),
            '/cursor is being used in a join/')

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: ic0.reset(),
            '/cursor is being used in a join/')

        # Only a small number of operations allowed on a join cursor
        msg = "/Unsupported cursor/"
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: jc.search(), msg)

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: jc.prev(), msg)

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
        self.session.create('table:join08', 'key_format=r' +
                            ',value_format=SS,columns=(k,v0,v1)')
        self.session.create('index:join08:index0','columns=(v0)')
        self.session.create('index:join08:index1','columns=(v1)')
        c = self.session.open_cursor('table:join08', None, None)
        for i in range(0, self.nentries):
            c.set_key(*self.gen_key(i))
            c.set_value(*self.gen_values(i))
            c.insert()
        c.close()

        if joinfirst:
            jc = self.session.open_cursor('join:table:join08', None, None)
        c0 = self.session.open_cursor('index:join08:index0', None, None)
        c1 = self.session.open_cursor('index:join08:index1', None, None)
        c0.next()        # index cursors must be positioned
        c1.next()
        if not joinfirst:
            jc = self.session.open_cursor('join:table:join08', None, None)
        self.session.join(jc, c0, 'compare=ge')
        self.session.join(jc, c1, 'compare=ge')
        self.session.close()
        self.session = None

    def test_cursor_close1(self):
        self.cursor_close_common(True)

    def test_cursor_close2(self):
        self.cursor_close_common(False)

    # test statistics with a simple one index join cursor
    def test_simple_stats(self):
        self.session.create("table:join01b",
                       "key_format=i,value_format=i,columns=(k,v)")
        self.session.create("index:join01b:index", "columns=(v)")

        cursor = self.session.open_cursor("table:join01b", None, None)
        cursor[1] = 11
        cursor[2] = 12
        cursor[3] = 13
        cursor.close()

        cursor = self.session.open_cursor("index:join01b:index", None, None)
        cursor.set_key(11)
        cursor.search()

        jcursor = self.session.open_cursor("join:table:join01b", None, None)
        self.session.join(jcursor, cursor, "compare=gt")

        while jcursor.next() == 0:
            [k] = jcursor.get_keys()
            [v] = jcursor.get_values()

        statcur = self.session.open_cursor("statistics:join", jcursor, None)
        found = False
        while statcur.next() == 0:
            [desc, pvalue, value] = statcur.get_values()
            #self.tty(str(desc) + "=" + str(pvalue))
            found = True
        self.assertEquals(found, True)

        jcursor.close()
        cursor.close()


if __name__ == '__main__':
    wttest.run()
