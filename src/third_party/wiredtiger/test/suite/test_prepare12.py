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
# [TEST_TAGS]
# eviction:prepare
# [END_TAGS]

import wiredtiger, wttest
from wtscenario import make_scenarios

# test_prepare12.py
# Test update restore of a page with prepared update.
class test_prepare12(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=2MB'
    session_config = 'isolation=snapshot'

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('integer_row', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def test_prepare_update_restore(self):
        uri = "table:test_prepare12"
        format = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, format)

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_aaa = 65
        else:
            value_a = 'a'
            value_b = 'b'
            value_aaa = 'a' * 500

        # Prepare a transaction
        cursor = self.session.open_cursor(uri, None)
        self.session.begin_transaction()
        cursor[1] = value_a
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(1))

        # Insert an uncommitted key
        session2 = self.conn.open_session(None)
        cursor2 = session2.open_cursor(uri, None)
        session2.begin_transaction()
        cursor2[2] = value_b

        # Insert a bunch of other content to fill the database to trigger eviction.
        session3 = self.conn.open_session(None)
        cursor3 = session3.open_cursor(uri, None)
        for i in range(3, 101):
            session3.begin_transaction()
            cursor3[i] = value_aaa
            session3.commit_transaction()

        # Commit the prepared update
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(1) + ',durable_timestamp=' + self.timestamp_str(2))

        # Read the prepared update
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(2))
        self.assertEqual(cursor[1], value_a)
