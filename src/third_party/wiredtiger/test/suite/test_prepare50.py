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
#
# With preserve_prepared configured, a prepared transaction's durable
# timestamp must be strictly greater than its prepare timestamp.

import wiredtiger, wttest

class test_prepare50(wttest.WiredTigerTestCase):

    test_name = __qualname__
    conn_config = 'precise_checkpoint=true,preserve_prepared=true'
    uri = f'table:{test_name}'

    def setUp(self):
        super().setUp()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))

    def test_durable_ts_equal_to_prepare_ts_fails(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        cursor[1] = 'value'
        self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(10) +
            ',prepared_id=' + self.prepared_id_str(1))
        self.session.timestamp_transaction(
            'commit_timestamp=' + self.timestamp_str(10))

        msg = '/durable timestamp.*must be greater than the prepare timestamp/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.timestamp_transaction(
                'durable_timestamp=' + self.timestamp_str(10)), msg)
        self.session.rollback_transaction()

    def test_durable_ts_greater_than_prepare_ts_succeeds(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        cursor[3] = 'value'
        self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(30) +
            ',prepared_id=' + self.prepared_id_str(3))
        self.session.timestamp_transaction(
            'commit_timestamp=' + self.timestamp_str(30))
        self.session.timestamp_transaction(
            'durable_timestamp=' + self.timestamp_str(31))
        self.session.commit_transaction()

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(31))
        self.assertEqual(cursor[3], 'value')
        self.session.commit_transaction()

if __name__ == '__main__':
    wttest.run()
