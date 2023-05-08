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
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_bug030.py
# This tests for the scenario in WT-10522 where we could return early when
# appending a key's original value to its update list due to checking some
# flags that must be ignored when looking at an aborted tombstone.
class test_bug_030(wttest.WiredTigerTestCase):
    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def conn_config(self):
        config = 'debug_mode=(update_restore_evict=true)'
        return config

    def test_bug030(self):
        nrows = 10
        uri = "table:test_bug030"

        if self.value_format == '8t':
            valuea = 97
            valueb = 98
        else:
            valuea = "abcdef" * 3
            valueb = "ghijkl" * 3

        self.session.create(uri, 'key_format={},value_format={}'.format(
            self.key_format, self.value_format))

        # Stable insert
        self.session.begin_transaction()
        cursor = self.session.open_cursor(uri)
        for i in range(1, nrows + 1):
            cursor[i] = valuea
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        self.conn.set_timestamp('oldest_timestamp={},stable_timestamp={}'.format(
            self.timestamp_str(10), self.timestamp_str(20)))

        # Unstable delete
        self.session.begin_transaction()
        cursor = self.session.open_cursor(uri)
        for i in range(1, nrows + 1):
            cursor.set_key(i)
            cursor.remove()
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        # Evict everything
        self.session.begin_transaction()
        evict_cursor = self.session.open_cursor(uri, None, 'debug=(release_evict)')
        for i in range(1, nrows + 1):
            evict_cursor.set_key(i)
            evict_cursor.search()
        evict_cursor.reset()
        evict_cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(40))

        # Unstable, uncommitted update
        self.session.begin_transaction()
        cursor = self.session.open_cursor(uri)
        for i in range(1, nrows + 1):
            cursor[i] = valueb
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(50))

        self.session.checkpoint()
        self.conn.rollback_to_stable()

        # Another delete, committed this time
        self.session.begin_transaction()
        cursor = self.session.open_cursor(uri)
        for i in range(1, nrows + 1):
            cursor.set_key(i)
            cursor.remove()
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(60))

        # Finally, evict everything. At this point, our key(s) have an
        # update chain with a single entry that's both aborted and restored
        # from the data store, that we attempt to reconcile.
        self.session.begin_transaction()
        evict_cursor = self.session.open_cursor(uri, None, 'debug=(release_evict)')
        for i in range(1, nrows + 1):
            evict_cursor.set_key(i)
            evict_cursor.search()
        evict_cursor.reset()
        evict_cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(70))
