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

from rollback_to_stable_util import test_rollback_to_stable_base
from helper import simulate_crash_restart
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_rollback_to_stable44.py
# Check that RTS backs out prepared transactions on recover if the stable timestamp is not set.

class test_rollback_to_stable44(test_rollback_to_stable_base):

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def conn_config(self):
        return 'verbose=(rts:5)'

    def evict(self, uri, session, key, value):
        evict_cursor = session.open_cursor(uri, None, "debug=(release_evict)")
        session.begin_transaction('ignore_prepare=true')
        v = evict_cursor[key]
        self.assertEqual(v, value)
        self.assertEqual(evict_cursor.reset(), 0)
        session.rollback_transaction()

    def test_rollback_to_stable(self):
        nrows = 10

        # Create a table.
        uri = "table:rollback_to_stable44"
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
        else:
            value_a = "aaaaa" * 10
            value_b = "bbbbb" * 10

        # Do not set the stable and the oldest timestamps.

        # Write aaaaaa to all the keys at time 10.
        self.large_updates(uri, value_a, ds, nrows, False, 10)

        # Write bbbbbb to the first key, prepare it, but do not commit.
        session_b = self.conn.open_session()
        session_b.begin_transaction()
        cursor_b = session_b.open_cursor(uri)
        cursor_b[ds.key(1)] = value_b
        session_b.prepare_transaction('prepare_timestamp=' + self.timestamp_str(20))

        # Evict the page with the first key.
        self.evict(uri, self.session, ds.key(1), value_a)

        # Checkpoint and simulate crash, which triggers RTS.
        self.session.checkpoint()
        simulate_crash_restart(self, '.', 'RESTART')

        # Check that the prepare operation did not have any effect.
        self.check(0, uri, 0, nrows, 5)
        self.check(value_a, uri, nrows, 0, 15)
        self.check(value_a, uri, nrows, 0, 25)
