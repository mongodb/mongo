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
from wtdataset import SimpleDataSet

# test_reconcile02.py
#
# Test removing existing deleted keys from the old disk image is
# considered making progress in reconciliation.
class test_reconcile02(wttest.WiredTigerTestCase):
    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('integer-row', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def test_reconcile02(self):
        uri = "table:test_reconcile02"

        # Create a table
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        # Insert two keys at timestamp 10
        cursor = self.session.open_cursor(uri, None, None)
        self.session.begin_transaction()
        cursor[1] = 'value'
        cursor[2] = 'value'
        self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")

        # Delete a key at timestamp 20
        self.session.begin_transaction()
        cursor.set_key(1)
        cursor.remove()
        self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(20)}")

        # Evict the page to force reconciliation.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        v = evict_cursor[2]
        self.assertEqual(v, 'value')
        self.assertEqual(evict_cursor.reset(), 0)
        self.session.rollback_transaction()

        # Do an uncommitted update
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(uri, None, None)
        session2.begin_transaction()
        cursor2[2] = 'value2'

        # Make the delete globally visible
        self.conn.set_timestamp(f"stable_timestamp={self.timestamp_str(20)},oldest_timestamp={self.timestamp_str(20)}")

        # Evict the page to force reconciliation.
        self.session.begin_transaction()
        v = evict_cursor[2]
        self.assertEqual(v, 'value')
        self.assertEqual(evict_cursor.reset(), 0)
        self.session.rollback_transaction()
        evict_cursor.close()

        # Verify that reconciliation made progress.
        stats_cursor = self.session.open_cursor('statistics:' + uri)
        self.assertEqual(stats_cursor[wiredtiger.stat.dsrc.cache_eviction_blocked_no_progress][2], 0)
        stats_cursor.close()
