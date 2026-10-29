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

import wiredtiger, wttest
from wtscenario import make_scenarios
from helper import WiredTigerCursor, statistic_uri

OTEL_TYPES = ['otel_counters', 'otel_gauges', 'otel_histograms', 'otel_none']

# Statistics cursors opened with an OTel type only expose statistics of that type.
class test_stat19(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all)'
    table_uri = 'table:test_stat19'

    uri = [
        ('connection', dict(uri='statistics:')),
        ('session', dict(uri='statistics:session')),
        ('table', dict(uri='statistics:table:test_stat19')),
    ]
    otel = [(t, dict(otel=t)) for t in OTEL_TYPES]
    scenarios = make_scenarios(uri, otel)

    def setUp(self):
        super().setUp()
        self.session.create(self.table_uri, 'key_format=S,value_format=S')

    def open(self, config='statistics=(all)'):
        return self.session.open_cursor(self.uri, None, config)

    def otel_config(self, mode='all'):
        return 'statistics=(' + mode + ',' + self.otel + ')'

    def walk(self, cursor, advance):
        keys = []
        while advance() == 0:
            keys.append(cursor.get_key())
        return keys

    def subset(self, mode='all'):
        cursor = self.open(self.otel_config(mode))
        keys = self.walk(cursor, cursor.next)
        cursor.close()
        return keys

    def full(self):
        cursor = self.open()
        keys = self.walk(cursor, cursor.next)
        cursor.close()
        return keys

    def test_prev_reverses_next(self):
        cursor = self.open(self.otel_config())
        forward = self.walk(cursor, cursor.next)
        cursor.reset()
        backward = self.walk(cursor, cursor.prev)
        cursor.close()
        self.assertEqual(forward, list(reversed(backward)))

    def test_search(self):
        keys = self.subset()
        full = self.full()
        cursor = self.open(self.otel_config())
        for key in keys:
            cursor.set_key(key)
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_key(), key)
        for key in set(full) - set(keys):
            cursor.set_key(key)
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        cursor.close()

    def test_search_out_of_range(self):
        full = self.full()
        cursor = self.open(self.otel_config())
        for key in [-1, 0, min(full) - 1, max(full) + 1, max(full) + 100000]:
            cursor.set_key(key)
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        cursor.close()

    def test_step_past_end(self):
        cursor = self.open(self.otel_config())
        keys = self.walk(cursor, cursor.next)
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
        if len(keys) > 1:
            self.assertEqual(cursor.prev(), 0)
            self.assertEqual(cursor.get_key(), keys[-2])
        cursor.close()

    def test_reset(self):
        cursor = self.open(self.otel_config())
        first = self.walk(cursor, cursor.next)
        cursor.reset()
        self.assertEqual(self.walk(cursor, cursor.next), first)
        cursor.close()

    def test_fast_equals_all(self):
        self.assertEqual(self.subset('fast'), self.subset('all'))

# The OTel types partition the statistics: no statistic has more than one type.
class test_stat19_disjoint(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all)'

    uri = [
        ('connection', dict(uri='statistics:')),
        ('session', dict(uri='statistics:session')),
    ]
    scenarios = make_scenarios(uri)

    def test_disjoint(self):
        seen = set()
        for otel in OTEL_TYPES:
            cursor = self.session.open_cursor(self.uri, None, 'statistics=(all,' + otel + ')')
            keys = set()
            while cursor.next() == 0:
                keys.add(cursor.get_key())
            cursor.close()
            self.assertEqual(seen & keys, set())
            seen |= keys

        cursor = self.session.open_cursor(self.uri, None, 'statistics=(all)')
        everything = set()
        while cursor.next() == 0:
            everything.add(cursor.get_key())
        cursor.close()
        self.assertEqual(seen, everything)

# Clearing through a cursor of one OTel type must not clear statistics of any other type.
class test_stat19_clear(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all)'
    uri = 'table:test_stat19_clear'

    scenarios = make_scenarios([('all', dict())])

    def read(self, key):
        with WiredTigerCursor(self.session, statistic_uri()) as stat_cur:
            stat_cur.set_key(key)
            self.assertEqual(stat_cur.search(), 0)
            return stat_cur.get_value()[2]

    def test_clear_only_otel_counters(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(self.uri)
        for i in range(100):
            cursor[str(i)] = str(10)
        while cursor.next() == 0:
            pass
        cursor.close()

        counter_before_clear = self.read(wiredtiger.stat.conn.cursor_insert)
        next_before_clear = self.read(wiredtiger.stat.conn.cursor_next)
        # If the statistics are 0, they can't be tested for clearing correctly.
        self.assertGreater(counter_before_clear, 0)
        self.assertGreater(next_before_clear, 0)

        with WiredTigerCursor(self.session, statistic_uri(), None,
                              'statistics=(all,clear,otel_counters)') as clearing:
            pass

        counter_after_clear = self.read(wiredtiger.stat.conn.cursor_insert)
        next_after_clear = self.read(wiredtiger.stat.conn.cursor_next)
        # Only the counter type should be cleared.
        self.assertEqual(next_after_clear, next_before_clear)
        self.assertLess(counter_after_clear, counter_before_clear)
        self.assertEqual(counter_after_clear, 0)

    def test_clear_only_otel_none(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(self.uri)
        for i in range(100):
            cursor[str(i)] = str(10)
        while cursor.next() == 0:
            pass
        cursor.close()

        counter_before_clear = self.read(wiredtiger.stat.conn.cursor_insert)
        next_before_clear = self.read(wiredtiger.stat.conn.cursor_next)
        # If the statistics are 0, they can't be tested for clearing correctly.
        self.assertGreater(counter_before_clear, 0)
        self.assertGreater(next_before_clear, 0)

        with WiredTigerCursor(self.session, statistic_uri(), None,
                              'statistics=(all,clear,otel_none)') as clearing:
            pass

        counter_after_clear = self.read(wiredtiger.stat.conn.cursor_insert)
        next_after_clear = self.read(wiredtiger.stat.conn.cursor_next)
        # Only the statistics without an OTel type should be cleared.
        self.assertEqual(counter_after_clear, counter_before_clear)
        self.assertLess(next_after_clear, next_before_clear)
