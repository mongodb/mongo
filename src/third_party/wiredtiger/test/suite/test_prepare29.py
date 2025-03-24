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

import wttest
from helper import simulate_crash_restart
from wtscenario import make_scenarios

# test_prepare29.py
# Test that updates preceding an unstable prepared tombstone are restored with the correct
# transaction IDs.
class test_prepare29(wttest.WiredTigerTestCase):

    format_values = [
        ('column', dict(key_format='r', key=1, value_format='S')),
        ('column-fix', dict(key_format='r', key=1, value_format='8t')),
        ('string-row', dict(key_format='S', key=str(1), value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def evict_cursor(self, uri, key):
        session_evict = self.conn.open_session("debug=(release_evict_page=true)")
        session_evict.begin_transaction("ignore_prepare=true")
        cursor = session_evict.open_cursor(uri, None, None)
        cursor.set_key(key)
        cursor.search()
        cursor.reset()
        cursor.close()
        session_evict.rollback_transaction()

    def test_prepare29(self):
        uri = 'table:test_prepare29'
        create_params = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, create_params)

        if self.value_format == '8t':
            value1 = 97
        else:
            value1 = 'a' * 5

        # Pin oldest timestamp.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))

        # Insert some data.
        ts = 100
        cursor = self.session.open_cursor(uri)
        key = self.key
        self.session.begin_transaction()
        cursor[key] = value1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))

        # Update stable timestamp and checkpoint.
        ts = 200
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(ts))
        self.session.checkpoint()

        # Remove the previously inserted key in a prepared transaction.
        ts = 300
        self.session.begin_transaction()
        cursor.set_key(key)
        cursor.remove()
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(ts))
        cursor.reset()
        cursor.close()

        # Evict the key.
        self.evict_cursor(uri, key)
        session = self.conn.open_session("")
        session.checkpoint()

        # Perform an unclean shutdown. The last remove operation is unstable and unresolved.
        simulate_crash_restart(self, '.', 'RESTART')

        # Try to perform an operation in a transaction that is after the stable timestamp.
        # We should get a WT_ROLLBACK error due to the write conflict if we don't properly
        # reset the txn ID.
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        cursor.set_key(key)
        cursor.remove()

        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(201))
