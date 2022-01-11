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
from helper import simulate_crash_restart

# test_prepare17.py
# The following test is to verify that if the out of order commit timestamp(for a transaction, say T2) lies between
# previous commit and durable timestamps(for a transaction, say T1), the durable timestamp of T1 changes to
# the commit timestamp of T2.
class test_prepare17(wttest.WiredTigerTestCase):
    uri = 'table:test_prepare17'
    nrows = 1000

    format_values = [
        ('integer-row', dict(key_format='i', value_format='S')),
        ('column', dict(key_format='r', value_format='S')),
        ('column-fix', dict(key_format='r', value_format='8t')),
    ]
    update = [
        ('prepare', dict(prepare=True)),
        ('non-prepare', dict(prepare=False)),
    ]
    scenarios = make_scenarios(format_values, update)

    def moresetup(self):
        if self.value_format == '8t':
            self.value1 = 97
            self.value2 = 98
        else:
            self.value1 = 'aaaaa'
            self.value2 = 'bbbbb'

    def test_prepare(self):
        self.moresetup()
        create_params = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(self.uri, create_params)
        cursor = self.session.open_cursor(self.uri)

        # Transaction one
        self.session.begin_transaction()
        for i in range(1, self.nrows + 1):
            cursor[i] = self.value1
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(2))
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(3)+ ',durable_timestamp=' + self.timestamp_str(6))

        # In the case below, the commit timestamp lies between the previous commit and durable timestamps.
        # Internally, WiredTiger changes the durable timestamp of Transaction one, i.e. 6 to the commit timestamp 
        # of the transaction below, i.e, 4.
        # As per the time window validation the commit timestamp cannot be in between any previous commit and 
        # durable timestamps.
        #
        # Note: The scenario where commit timestamp lies between the previous commit and durable timestamps
        # is not expected from MongoDB, but WiredTiger API can allow it.
        if self.prepare:
            self.session.begin_transaction()
            for i in range(1, self.nrows + 1):
                cursor[i] = self.value2
            self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(3))
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(4) + ',durable_timestamp=' + self.timestamp_str(7))
        else:
            self.session.begin_transaction()
            for i in range(1, self.nrows + 1):
                cursor[i] = self.value2
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(4))

        # Time window validation occurs as part of checkpoint.
        self.session.checkpoint()

    def test_prepare_insert_remove(self):
        self.moresetup()
        if not self.prepare:
            return

        create_params = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(self.uri, create_params)
        cursor = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        for i in range(1, self.nrows + 1):
            cursor[i] = self.value1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

        # Transaction 2
        self.session.begin_transaction()
        for i in range(1, self.nrows + 1):
            cursor[i] = self.value2
            cursor.set_key(i)
            cursor.remove()
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(3))
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(4) + ',durable_timestamp=' + self.timestamp_str(7))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(6))
        self.session.checkpoint()
        simulate_crash_restart(self, ".", "RESTART")

        # Update in Transaction2 should be removed as the stable timestamp (6) is less than the durable timestamp (7).
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, self.nrows + 1):
            cursor.set_key(i)
            cursor.search()
            self.assertEqual(cursor.get_value(), self.value1)
        self.session.rollback_transaction()
