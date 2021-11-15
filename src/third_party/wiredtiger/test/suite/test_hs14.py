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

import time, wiredtiger, wttest
from wtscenario import make_scenarios

# test_hs14.py
# Ensure that point in time reads with few visible history store records don't
# damage performance.
class test_hs14(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB'
    session_config = 'isolation=snapshot'
    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column-fix', dict(key_format='r', value_format='8t')),
        ('string-row', dict(key_format='S', value_format='S'))
    ]
    scenarios = make_scenarios(format_values)

    def create_key(self, i):
        if self.key_format == 'S':
            return str(i)
        return i

    def test_hs14(self):
        uri = 'table:test_hs14'
        config = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, config)
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        cursor = self.session.open_cursor(uri)

        nrows = 10000

        if self.value_format == '8t':
            value1 = 97
            value2 = 98
            value3 = 99
            value4 = 100
            value5 = 101
        else:
            value1 = 'a' * 500
            value2 = 'b' * 500
            value3 = 'c' * 500
            value4 = 'd' * 500
            value5 = 'e' * 500

        for i in range(1, nrows):
            self.session.begin_transaction()
            cursor[self.create_key(i)] = value1
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))
            self.session.begin_transaction()
            cursor[self.create_key(i)] = value2
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))
            self.session.begin_transaction()
            cursor[self.create_key(i)] = value3
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(3))
            self.session.begin_transaction()
            cursor[self.create_key(i)] = value4
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(4))

        # A checkpoint will ensure that older values are written to the history store.
        self.session.checkpoint()

        start = time.time()
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(3))
        for i in range(1, nrows):
            self.assertEqual(cursor[self.create_key(i)], value3)
        self.session.rollback_transaction()
        end = time.time()

        # The time spent when all history store keys are visible to us.
        visible_hs_latency = (end - start)

        for i in range(1, nrows):
            self.session.begin_transaction()
            cursor.set_key(self.create_key(i))
            cursor.remove()
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(5))
            self.session.begin_transaction()
            cursor[self.create_key(i)] = value5
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        # A checkpoint will ensure that older values are written to the history store.
        self.session.checkpoint()

        start = time.time()
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(9))
        for i in range(1, nrows):
            cursor.set_key(self.create_key(i))
            if self.value_format == '8t':
                self.assertEqual(cursor.search(), 0)
                self.assertEqual(cursor.get_value(), 0)
            else:
                self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        self.session.rollback_transaction()
        end = time.time()

        # The time spent when all history store keys are invisible to us.
        invisible_hs_latency = (end - start)

        self.assertLess(invisible_hs_latency, (visible_hs_latency * 10),
                        "Reader took an order of magnitude longer for when all "
                        "history store records were invisible, visible={}, invisible={}".format(
                            visible_hs_latency, invisible_hs_latency
                        ))
