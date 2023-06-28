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
from wtscenario import make_scenarios

# test_checkpoint06.py
# Verify that we rollback the truncation that is committed after stable
# timestamp in the checkpoint.
class test_checkpoint06(wttest.WiredTigerTestCase):
    conn_config = 'create,cache_size=10MB'

    format_values = [
        ('column-fix', dict(key_format='r', value_format='8t')),
        ('column', dict(key_format='r', value_format='S')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    prepare_values = [
        ('prepare', dict(prepare=True)),
        ('no_prepare', dict(prepare=False)),
    ]

    scenarios = make_scenarios(format_values, prepare_values)

    def test_rollback_truncation_in_checkpoint(self):
        nrows = 10000
        self.uri = 'table:ckpt06'
        self.uri_evict = 'table:ckpt06_evict'
        config = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(self.uri, config)
        self.session.create(self.uri_evict, config)

        if self.value_format == '8t':
            value = 72
        else:
            value = "abcdefghijklmnopqrstuvwxyz" * 3

        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        cursor = self.session.open_cursor(self.uri)
        # Setup: Insert some data
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor[i] = value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))
        cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(2))

        # Flush everything to disk
        self.reopen_conn()

        # Truncate large portion of the data in the table
        self.session.begin_transaction()
        start = self.session.open_cursor(self.uri)
        start.set_key(5)
        self.session.truncate(None, start, None, None)
        if self.prepare:
            self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(3))
            self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(3))
            self.session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(5))
            self.session.commit_transaction()
        else:
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(3))

        # Increase the stable timestamp to 4 which is less than truncate durable timestamp.
        if self.prepare:
            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(4) +
            ',stable_timestamp=' + self.timestamp_str(4))

        cursor = self.session.open_cursor(self.uri_evict)
        # Insert some more data into another table to trigger eviction
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor[i] = value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(6))
        cursor.close()

        # Do a checkpoint
        self.session.checkpoint()

        # rollback to stable
        self.conn.rollback_to_stable()

        # Verify the truncation is rolled back.
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, nrows + 1):
            self.assertEqual(cursor[i], value)
        cursor.close()
