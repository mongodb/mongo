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

import wiredtiger, wttest, time
from wiredtiger import stat
from wtscenario import make_scenarios
import test_cursor01, test_cursor02, test_cursor03
import test_checkpoint01, test_checkpoint02
from wtdataset import SimpleDataSet, ComplexDataSet
from helper import confirm_does_not_exist
from suite_random import suite_random

# Cursor caching tests
#
# This test uses only row-store (key_format='S') but the cursor-caching code has been reviewed
# for dependence on the access method and found to be access-method independent, so rearranging
# it to also test column-store is not necessary.
class test_cursor13_base(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(fast)'
    stat_cursor_cache = 0
    stat_cursor_reopen = 0

    # Returns a list: [cursor_cached, cursor_reopened]
    #
    # We want the statistics for operations triggered from our program. The challenge is that
    # eviction threads may cache history store cursors in the background. We address this by
    # subtracting out operations from the history store file. This is tricky because we can't
    # atomically check the connections stats and the history store stats.  So we look at the
    # history store stats before and after the connection stats and only accept a result where
    # the history store stats haven't changed.
    def caching_stats(self):
        hs_stats_uri = 'statistics:file:WiredTigerHS.wt'
        max_tries = 100
        for i in range(max_tries):
            hs_stats_before = self.session.open_cursor(hs_stats_uri, None, None)
            conn_stats = self.session.open_cursor('statistics:', None, None)
            hs_stats_after = self.session.open_cursor(hs_stats_uri, None, None)

            totals = [ conn_stats [stat.conn.cursor_cache][2],
                         conn_stats [stat.conn.cursor_reopen][2] ]
            hs_before = [ hs_stats_before[stat.dsrc.cursor_cache][2],
                          hs_stats_before[stat.dsrc.cursor_reopen][2] ]
            hs_after = [ hs_stats_after[stat.dsrc.cursor_cache][2],
                         hs_stats_after[stat.dsrc.cursor_reopen][2] ]

            hs_stats_before.close()
            hs_stats_after.close()
            conn_stats.close()

            if hs_before[0] == hs_after[0] and hs_before[1] == hs_after[1]:
                break

            # Fail if we haven't been able to get stable history store stats after too many attempts.
            # Seems impossible, but better to check than to have an accidental infinite loop.
            self.assertNotEqual(i, max_tries - 1)

        return [totals[0] - hs_after[0], totals[1] - hs_after[1]]

    # Returns a list: [cursor_sweep, cursor_sweep_buckets,
    #                  cursor_sweep_examined, cursor_sweep_closed]
    def sweep_stats(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        sweep = stat_cursor[stat.conn.cursor_sweep][2]
        buckets = stat_cursor[stat.conn.cursor_sweep_buckets][2]
        examined = stat_cursor[stat.conn.cursor_sweep_examined][2]
        closed = stat_cursor[stat.conn.cursor_sweep_closed][2]
        stat_cursor.close()
        return [sweep, buckets, examined, closed]

    def assert_cursor_cached(self, expect_change):
        stats = self.caching_stats()
        if expect_change:
            self.assertGreater(stats[0], self.stat_cursor_cache)
            self.stat_cursor_cache = stats[0]
        else:
            self.assertEqual(stats[0], self.stat_cursor_cache)

    def assert_cursor_reopened(self, expect_change):
        stats = self.caching_stats()
        if expect_change:
            self.assertGreater(stats[1], self.stat_cursor_reopen)
            self.stat_cursor_reopen = stats[1]
        else:
            self.assertEqual(stats[1], self.stat_cursor_reopen)

    def cursor_stats_init(self):
        stats = self.caching_stats()
        self.stat_cursor_cache = stats[0]
        self.stat_cursor_reopen = stats[1]

# Override other cursor tests with cursors cached.
class test_cursor13_01(test_cursor01.test_cursor01, test_cursor13_base):
    pass

class test_cursor13_02(test_cursor02.test_cursor02, test_cursor13_base):
    pass

class test_cursor13_03(test_cursor03.test_cursor03, test_cursor13_base):
    pass

class test_cursor13_ckpt01(test_checkpoint01.test_checkpoint,
                           test_cursor13_base):
    pass

class test_cursor13_ckpt02(test_checkpoint01.test_checkpoint_cursor,
                           test_cursor13_base):
    pass

class test_cursor13_ckpt03(test_checkpoint01.test_checkpoint_target,
                           test_cursor13_base):
    pass

class test_cursor13_ckpt04(test_checkpoint01.test_checkpoint_cursor_update,
                           test_cursor13_base):
    pass

class test_cursor13_ckpt05(test_checkpoint01.test_checkpoint_last,
                           test_cursor13_base):
    pass

class test_cursor13_ckpt06(test_checkpoint01.test_checkpoint_empty,
                           test_cursor13_base):
    pass

class test_cursor13_ckpt2(test_checkpoint02.test_checkpoint02,
                          test_cursor13_base):
    pass

class test_cursor13_reopens(test_cursor13_base):
    # The SimpleDataSet uses simple tables, that have no column groups or
    # indices. Thus, these tables will be cached. The more complex data sets
    # are not simple, so are not cached and not included in this test.
    types = [
        ('file', dict(uri='file:cursor13_reopen1', dstype=None)),
        ('table', dict(uri='table:cursor13_reopen2', dstype=None)),
        ('sfile', dict(uri='file:cursor13_reopen3', dstype=SimpleDataSet)),
        ('stable', dict(uri='table:cursor13_reopen4', dstype=SimpleDataSet)),
    ]
    connoptions = [
        ('none', dict(connoption='', conn_caching=None)),
        ('enable', dict(connoption='cache_cursors=true', conn_caching=True)),
        ('disable', dict(connoption='cache_cursors=false', conn_caching=False)),
    ]
    sessoptions = [
        ('none', dict(sessoption='', sess_caching=None)),
        ('enable', dict(sessoption='cache_cursors=true', sess_caching=True)),
        ('disable', dict(sessoption='cache_cursors=false', sess_caching=False)),
    ]
    scenarios = make_scenarios(types, connoptions, sessoptions)

    def conn_config(self):
        return self.connoption + ',statistics=(fast)'

    def session_config(self):
        return self.sessoption

    def basic_populate(self, uri, caching_enabled):
        cursor = self.session.open_cursor(uri)
        cursor['A'] = 'B'
        cursor.close()
        self.assert_cursor_cached(caching_enabled)
        cursor = self.session.open_cursor(uri)
        self.assert_cursor_reopened(caching_enabled)
        cursor['B'] = 'C'
        cursor.close()
        self.assert_cursor_cached(caching_enabled)

    def basic_check(self, cursor):
        count = 0
        for x,y in cursor:
            if count == 0:
                self.assertEqual('A', x)
                self.assertEqual('B', y)
            elif count == 1:
                self.assertEqual('B', x)
                self.assertEqual('C', y)
            count += 1
        self.assertEqual(count, 2)

    def basic_reopen(self, nopens, create, caching_enabled):
        session = self.session
        if create:
            session.create(self.uri, 'key_format=S,value_format=S')
            self.basic_populate(self.uri, caching_enabled)
            # At this point, we've cached one cursor.

        # Reopen/close many times, with multiple cursors
        for opens in range(0, nopens):
            # We expect a cursor to be reopened if we did the
            # create operation above or if this is the second or later
            # time through the loop.
            c = session.open_cursor(self.uri)
            self.assert_cursor_reopened(caching_enabled and (opens != 0 or create))

            # With one cursor for this URI already open, we'll only
            # get a reopened cursor if this is the second or later
            # time through the loop.
            c2 = session.open_cursor(self.uri)
            self.assert_cursor_reopened(caching_enabled and opens != 0)

            self.basic_check(c)
            self.basic_check(c2)
            c.close()
            self.assert_cursor_cached(caching_enabled)
            c2.close()
            self.assert_cursor_cached(caching_enabled)

    def dataset_reopen(self, caching_enabled):
        ds = self.dstype(self, self.uri, 100)
        ds.populate()
        self.assert_cursor_cached(caching_enabled)
        ds.check()
        self.assert_cursor_reopened(caching_enabled)

    # Return if caching was configured at the start of the session
    def is_caching_configured(self):
        if self.sess_caching != None:
            return self.sess_caching
        if self.conn_caching != None:
            return self.conn_caching
        return True   # default

    def test_reopen(self):
        caching = self.is_caching_configured()
        self.cursor_stats_init()
        if self.dstype == None:
            self.basic_reopen(100, True, caching)
        else:
            self.dataset_reopen(caching)

    def test_reconfig(self):
        caching = self.is_caching_configured()
        self.cursor_stats_init()
        self.basic_reopen(10, True, caching)
        self.session.reconfigure('cache_cursors=false')
        self.cursor_stats_init()
        self.basic_reopen(10, False, False)
        self.session.reconfigure('cache_cursors=true')
        self.cursor_stats_init()
        self.basic_reopen(10, False, True)

    # Test we can reopen across a verify.
    def test_verify(self):
        if self.dstype != None:
            self.session.reconfigure('cache_cursors=true')
            ds = self.dstype(self, self.uri, 100)
            ds.populate()
            for loop in range(10):
                # We need an extra cursor open to test all code paths in
                # this loop.  After the verify (the second or more time through
                # the loop), the data handle referred to by both cached
                # cursors will no longer be open.
                #
                # The first cursor open will attempt to reopen the
                # first cached cursor, will see the data handle closed,
                # thus will close that cursor and open normally.
                #
                # The second cursor open (in ds.check()) will attempt the
                # reopen the second cached cursor, see the data handle now
                # open and will succeed the reopen.
                #
                # This test checks that reopens of cursor using an already
                # reopened data handle will work.
                c = self.session.open_cursor(self.uri)
                ds.check()
                c.close()
                s2 = self.conn.open_session()
                s2.verify(self.uri)
                s2.close()

class test_cursor13_drops(test_cursor13_base):
    def open_and_drop(self, uri, cursor_session, drop_session, nopens, ntrials):
        for i in range(0, ntrials):
            cursor_session.create(uri, 'key_format=S,value_format=S')
            for i in range(0, nopens):
                c = cursor_session.open_cursor(uri)
                c.close()
            # The cursor cache is unaffected by the drop, and nothing
            # in the cache should prevent the drop from occurring.
            drop_session.drop(uri)
            confirm_does_not_exist(self, uri)

    def test_open_and_drop(self):
        session = self.session
        for uri in [ 'file:test_cursor13_drops', 'table:test_cursor13_drops' ]:
            self.open_and_drop(uri, session, session, 0, 5)
            self.open_and_drop(uri, session, session, 1, 5)
            self.open_and_drop(uri, session, session, 3, 5)

            # It should still work with different sessions
            session2 = self.conn.open_session(None)
            self.open_and_drop(uri, session2, session, 0, 5)
            self.open_and_drop(uri, session2, session, 1, 5)
            self.open_and_drop(uri, session2, session, 3, 5)
            session2.close()

    def test_open_index_and_drop(self):
        # We should also be able to detect cached cursors
        # for indices
        session = self.session
        uri = 'table:test_cursor13_drops'
        ds = ComplexDataSet(self, uri, 100)
        ds.create()
        indexname = ds.index_name(0)
        c = session.open_cursor(indexname)
        # The index is really open, so we cannot drop the main table.
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: session.drop(uri))
        c.close()
        session.drop(uri)
        confirm_does_not_exist(self, uri)

        # Same test for indices, but with cursor held by another session.
        # TODO: try with session that DOES have cache_cursors and another
        # that does not.
        session2 = self.conn.open_session(None)
        ds = ComplexDataSet(self, uri, 100)
        ds.create()
        indexname = ds.index_name(0)
        c = session2.open_cursor(indexname)
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: session.drop(uri))
        c.close()
        session.drop(uri)
        confirm_does_not_exist(self, uri)
        session2.close()

    def test_cursor_drops(self):
        session = self.session
        uri = 'table:test_cursor13_drops'
        idxuri = 'index:test_cursor13_drops:index1'
        config = 'key_format=S,value_format=S,columns=(k,v1)'

        for i in range(0, 2):
            session.create(uri, config)
            session.drop(uri)

        for i in range(0, 2):
            session.create(uri, config)
            cursor = session.open_cursor(uri, None)
            cursor['A'] = 'B'
            self.assertRaises(wiredtiger.WiredTigerError,
                lambda: session.drop(uri))
            cursor.close()
            session.drop(uri)

        for i in range(0, 2):
            session.create(uri, config)
            session.create(idxuri, 'columns=(v1)')
            cursor = session.open_cursor(uri, None)
            cursor['A'] = 'B'
            self.assertRaises(wiredtiger.WiredTigerError,
                lambda: session.drop(uri))
            cursor.close()
            session.drop(uri)

        for i in range(0, 2):
            session.create(uri, config)
            session.create(idxuri, 'columns=(v1)')
            cursor = session.open_cursor(uri, None)
            cursor['A'] = 'B'
            cursor.close()
            cursor = session.open_cursor(idxuri, None)
            self.assertRaises(wiredtiger.WiredTigerError,
                lambda: session.drop(uri))
            cursor.close()
            session.drop(uri)

# Shared base class for some bigger tests.
class test_cursor13_big_base(test_cursor13_base):
    deep = 3
    nuris = 100
    opencount = 0
    closecount = 0
    uri = None   # set by child classes

    def uriname(self, i):
        return self.uri + '.' + str(i)

    # Create a large number (self.nuris) of uris, and for each one,
    # create some number (self.deep) of cached cursors.
    def create_uri_map(self, baseuri):
        uri_map = {}
        for i in range(0, self.nuris):
            uri = self.uriname(i)
            cursors = []
            self.session.create(uri, None)
            for j in range(0, self.deep):
                cursors.append(self.session.open_cursor(uri, None))
            for c in cursors:
                c.close()
            # Each map entry has a list of the open cursors.
            # Since we just closed them, we start with none
            uri_map[uri] = []

        return uri_map

    def close_uris(self, uri_map, range_arg):
        closed = 0
        for i in range_arg:
            cursors = uri_map[self.uriname(i)]
            while len(cursors) > 0:
                cursors.pop().close()
                self.closecount += 1
                closed += 1
        return closed

    def open_or_close(self, uri_map, rand, low, high):
        uri = self.uriname(rand.rand_range(low, high))
        cursors = uri_map[uri]
        ncursors = len(cursors)

        # Keep the range of open cursors between 0 and [deep],
        # with some random fluctuation
        if ncursors == 0:
            do_open = True
        elif ncursors == self.deep:
            do_open = False
        else:
            do_open = (rand.rand_range(0, 2) == 0)

        if do_open:
            cursors.append(self.session.open_cursor(uri, None))
            self.opencount += 1
        else:
            i = rand.rand_range(0, ncursors)
            cursors.pop(i).close()
            self.closecount += 1

class test_cursor13_big(test_cursor13_big_base):
    scenarios = make_scenarios([
        ('file', dict(uri='file:cursor13_sweep_a')),
        ('table', dict(uri='table:cursor13_sweep_b'))
    ])

    nopens = 500000

    def test_cursor_big(self):
        rand = suite_random()
        uri_map = self.create_uri_map(self.uri)
        self.cursor_stats_init()
        begin_stats = self.caching_stats()
        #self.tty('stats before = ' + str(begin_stats))

        # At this point, we'll randomly open/close lots of cursors, keeping
        # track of how many of each. As long as we don't have more than [deep]
        # cursors open for each uri, we should always be taking then from
        # the set of cached cursors.
        while self.opencount < self.nopens:
            self.open_or_close(uri_map, rand, 0, self.nuris)

        end_stats = self.caching_stats()

        #self.tty('opens = ' + str(self.opencount) + \
        #         ', closes = ' + str(self.closecount))
        #self.tty('stats after = ' + str(end_stats))

        self.assertEquals(end_stats[0] - begin_stats[0], self.closecount)
        self.assertEquals(end_stats[1] - begin_stats[1], self.opencount)

class test_cursor13_sweep(test_cursor13_big_base):
    # Set dhandle sweep configuration so that dhandles should be closed within
    # two seconds of all the cursors for the dhandle being closed (cached).
    conn_config = 'statistics=(fast),' + \
                  'file_manager=(close_scan_interval=1,close_idle_time=1,' + \
                  'close_handle_minimum=0)'
    uri = 'table:cursor13_sweep_b'
    opens_per_round = 100000
    rounds = 5

    @wttest.longtest('cursor sweep tests require wait times')
    def test_cursor_sweep(self):
        rand = suite_random()

        uri_map = self.create_uri_map(self.uri)
        self.cursor_stats_init()
        begin_stats = self.caching_stats()
        begin_sweep_stats = self.sweep_stats()
        #self.tty('stats before = ' + str(begin_stats))
        #self.tty('sweep stats before = ' + str(begin_sweep_stats))
        potential_dead = 0

        for round_cnt in range(0, self.rounds):
            if round_cnt % 2 == 1:
                # Close cursors in half of the range, and don't
                # use them during this round, so they will be
                # closed by sweep.
                half = self.nuris // 2
                potential_dead += self.close_uris(uri_map, list(range(0, half)))
                bottom_range = half
                # Let the dhandle sweep run and find the closed cursors.
                time.sleep(3.0)
            else:
                bottom_range = 0

            # The session cursor sweep runs at most once a second and
            # traverses a fraction of the cached cursors.  We'll run for
            # ten seconds with pauses to make sure we see sweep activity.
            pause_point = self.opens_per_round // 100
            if pause_point == 0:
                pause_point = 1
            pause_duration = 0.1

            i = 0
            while self.opencount < (1 + round_cnt) * self.opens_per_round:
                i += 1
                if i % pause_point == 0:
                    time.sleep(pause_duration)   # over time, let sweep run
                self.open_or_close(uri_map, rand, bottom_range, self.nuris)

        end_stats = self.caching_stats()
        end_sweep_stats = self.sweep_stats()

        #self.tty('opens = ' + str(self.opencount) + \
        #         ', closes = ' + str(self.closecount))
        #self.tty('stats after = ' + str(end_stats))
        #self.tty('sweep stats after = ' + str(end_sweep_stats))
        self.assertEquals(end_stats[0] - begin_stats[0], self.closecount)
        swept = end_sweep_stats[3] - begin_sweep_stats[3]

        # Although this is subject to tuning parameters, we know that
        # in an active session, we'll sweep through minimum of 1% of
        # the cached cursors per second.  We've set this test to run
        # 5 rounds. In 2 of the 5 rounds (sandwiched between the others),
        # some of the uris are allowed to close. So during the 'closing rounds'
        # we'll sweep a minimum of 20% of the uri space, and in the other
        # rounds we'll be referencing the closed uris again.

        # We'll pass the test if we see at least 20% of the 'potentially
        # dead' cursors swept.  There may be more, since the 1% per second
        # is a minimum.
        min_swept = 2 * potential_dead // 10
        self.assertGreaterEqual(swept, min_swept)

        # No strict equality test for the reopen stats. When we've swept
        # some closed cursors, we'll have fewer reopens. It's different
        # by approximately the number of swept cursors, but it's less
        # predictable.
        self.assertGreater(end_stats[1] - begin_stats[1], 0)

class test_cursor13_dup(test_cursor13_base):
    def test_dup(self):
        self.cursor_stats_init()
        uri = 'table:test_cursor13_dup'
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri)
        cursor['A'] = 'B'
        cursor.close()

        # Get a cursor and position it.
        # An unpositioned cursor cannot be duplicated.
        c1 = self.session.open_cursor(uri, None)
        c1.next()

        for notused in range(0, 100):
            c2 = self.session.open_cursor(None, c1, None)
            c2.close()
        stats = self.caching_stats()
        self.assertGreaterEqual(stats[0], 100)  # cursor_cached > 100
        self.assertGreaterEqual(stats[1], 100)  # cursor_reopened > 100
