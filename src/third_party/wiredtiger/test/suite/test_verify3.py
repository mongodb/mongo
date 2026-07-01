#!/usr/bin/env python3
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

# test_verify3.py
#    When a deletion (or any other zero-sized value) becomes globally visible, reconciliation omits the on-page
#    value cell. History store verification used to advance the row index once per value cell,
#    so an omitted value cell made every following time window access out of sync.
class test_verify3(wttest.WiredTigerTestCase):
    uri = 'table:test_verify3'

    def test_verify_omitted_value_cell(self):
        self.session.create(self.uri, 'key_format=S,value_format=u')
        self.conn.set_timestamp('oldest_timestamp=1,stable_timestamp=1')

        c = self.session.open_cursor(self.uri)

        # k0: an empty, globally visible value, so its on-page value cell is omitted.
        self.session.begin_transaction()
        c['k0'] = b''
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

        # k1: an older version destined for the history store with stop timestamp 30.
        self.session.begin_transaction()
        c['k1'] = b'a'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        self.session.begin_transaction()
        c['k1'] = b'A'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        # k2: a single version with a start timestamp between k1's two versions.
        self.session.begin_transaction()
        c['k2'] = b'b'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        c.close()

        # Move oldest past k0 so its value cell is omitted, but below k1's history store stop so
        # that record survives, then make everything stable and checkpoint.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(5))
        self.session.checkpoint()

        # Every row is now paired with the right time window, so verification succeeds.
        self.assertEqual(self.session.verify(self.uri, None), 0)
