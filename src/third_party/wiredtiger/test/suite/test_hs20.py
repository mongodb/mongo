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

# test_hs20.py
# Ensure we never reconstruct a reverse modify update in the history store based on the onpage overflow value
class test_hs20(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB,eviction=(threads_max=1)'

    # Return the k'th (0-based) key.
    def make_column_key(k):
        return k + 1
    def make_string_key(k):
        return str(k)

    key_format_values = [
        ('column', dict(key_format='r', make_key=make_column_key)),
        ('string-row', dict(key_format='S', make_key=make_string_key)),
    ]

    scenarios = make_scenarios(key_format_values)

    def test_hs20(self):
        uri = 'table:test_hs20'
        key_format = 'key_format=' + self.key_format

        # Set a very small maximum leaf value to trigger writing overflow values
        self.session.create(uri, '{},value_format=S,leaf_value_max=10B'.format(key_format))
        cursor = self.session.open_cursor(uri)
        self.conn.set_timestamp(
            'oldest_timestamp=' + self.timestamp_str(1) + ',stable_timestamp=' + self.timestamp_str(1))

        value1 = 'a' * 500
        value2 = 'b' * 50

        # FIXME-WT-9063 revisit the use of self.retry() throughout this file.

        # Insert a value that is larger than the maximum leaf value.
        for i in range(0, 10):
            for retry in self.retry():
                with retry.transaction(commit_timestamp = 2):
                    cursor[self.make_key(i)] = value1

        # Do 2 modifies.
        for i in range(0, 10):
            for retry in self.retry():
                with retry.transaction(commit_timestamp = 3):
                    cursor.set_key(self.make_key(i))
                    mods = [wiredtiger.Modify('B', 500, 1)]
                    self.assertEqual(cursor.modify(mods), 0)

        for i in range(0, 10):
            for retry in self.retry():
                with retry.transaction(commit_timestamp = 4):
                    cursor.set_key(self.make_key(i))
                    mods = [wiredtiger.Modify('C', 501, 1)]
                    self.assertEqual(cursor.modify(mods), 0)

        # Insert more data to trigger eviction.
        for i in range(10, 100000):
            for retry in self.retry():
                with retry.transaction(commit_timestamp = 5):
                    cursor[self.make_key(i)] = value2

        # Update the overflow values.
        for i in range(0, 10):
            for retry in self.retry():
                with retry.transaction(commit_timestamp = 5):
                    cursor[self.make_key(i)] = value2

        # Do a checkpoint to move the overflow values to the history store but keep the current in memory disk image.
        self.session.checkpoint()

        # Search the first modifies.
        for i in range(0, 10):
            for retry in self.retry():
                with retry.transaction(read_timestamp = 3, rollback = True):
                    self.assertEqual(cursor[self.make_key(i)], value1 + "B")
