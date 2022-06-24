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
#
# test_cursor21.py
#   Test cursor reposition

import wttest
from wtscenario import make_scenarios
from wiredtiger import stat

class test_cursor21(wttest.WiredTigerTestCase):
    uri = "table:test_cursor21"

    format_values = [
        ('column', dict(key_format='r', value_format='i')),
        ('row_integer', dict(key_format='i', value_format='i')),
    ]
    reposition_values = [
        ('no_reposition', dict(reposition=False)),
        ('reposition', dict(reposition=True))
    ]
    scenarios = make_scenarios(format_values, reposition_values)

    def conn_config(self):
        config='cache_size=100MB,statistics=(all)'
        if self.reposition:
            config += ',debug_mode=[cursor_reposition=true],timing_stress_for_test=(evict_reposition)'
        return config

    def get_stat(self, stat, local_session = None):
        if (local_session != None):
            stat_cursor = local_session.open_cursor('statistics:')
        else:
            stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def check_reposition(self, count):
        reposition_count = self.get_stat(stat.conn.cursor_reposition, self.session)
        if self.reposition:
            count = reposition_count - count
            # Ensure that the reposition stat is greater than 0, indicating that repositon happened.
            self.assertGreater(count, 0)
        else:
            self.assertEqual(reposition_count, 0)
        return reposition_count

    def test_cursor21(self):
        format = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        reposition_count = 0
        self.session.create(self.uri, format)
        cursor = self.session.open_cursor(self.uri)

        # insert
        self.session.begin_transaction()
        for i in range(1, 10000):
            cursor[i] = i
        self.session.commit_transaction()

        # next
        self.session.begin_transaction()
        for i in range(1, 10000):
            cursor.next()
            self.assertEqual(cursor.get_value(), i)
        self.session.commit_transaction()

        reposition_count = self.check_reposition(reposition_count)
        cursor.reset()

        # prev
        self.session.begin_transaction()
        for i in range(9999, 0, -1):
            cursor.prev()
            self.assertEqual(cursor.get_value(), i)
        self.session.commit_transaction()

        reposition_count = self.check_reposition(reposition_count)
        cursor.reset()

        # search
        self.session.begin_transaction()
        for i in range(1, 10000):
            cursor.set_key(i)
            cursor.search()
            self.assertEqual(cursor.get_value(), i)
        self.session.commit_transaction()

        reposition_count = self.check_reposition(reposition_count)
        cursor.reset()

        # search_near
        self.session.begin_transaction()
        for i in range(1, 10000):
            cursor.set_key(i)
            cursor.search_near()
            self.assertEqual(cursor.get_value(), i)
        self.session.commit_transaction()

        reposition_count += self.check_reposition(reposition_count)
        cursor.close()
        self.session.close()
