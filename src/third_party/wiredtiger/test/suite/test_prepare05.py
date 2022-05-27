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
# test_prepare05.py
#   Prepare: Timestamps validation for prepare API's
#

from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wtscenario import make_scenarios

class test_prepare05(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_prepare05'
    uri = 'table:' + tablename

    format_values = [
        ('column', dict(key_format='r', value_format='i')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='i')),
    ]

    scenarios = make_scenarios(format_values)

    def test_timestamp_api(self):
        format = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(self.uri, format)
        c = self.session.open_cursor(self.uri)

        # It is illegal to set a prepare timestamp older than the stable timestamp.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(2))
        self.session.begin_transaction()
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(1)),
            "/not newer than the stable timestamp/")
        self.session.rollback_transaction()

        # It is also illegal to set a prepare timestamp the same as the stable timestamp.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(2))
        self.session.begin_transaction()
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(2)),
            "/not newer than the stable timestamp/")
        self.session.rollback_transaction()

        # Check setting the prepare timestamp immediately after the stable timestamp is valid.
        self.session.begin_transaction()
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(3))
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(3))
        self.session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(3))
        self.session.commit_transaction()

        # In a single transaction it is illegal to set a commit timestamp
        # before invoking prepare for this transaction.
        # Note: Values are not important, setting commit timestamp before
        # prepare itself is illegal.
        self.session.begin_transaction()
        self.session.timestamp_transaction(
            'commit_timestamp=' + self.timestamp_str(3))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(2)),
            "/should not have been set before/")
        self.session.rollback_transaction()

        # This is also true even if the prepare timestamp was set first.
        self.session.begin_transaction()
        self.session.timestamp_transaction('prepare_timestamp=' + self.timestamp_str(3))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:
                self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(3)),
            "/commit timestamp must not be set/")
        self.session.rollback_transaction()

        # It is legal to set a commit timestamp as same as prepare
        # timestamp.
        self.session.begin_transaction()
        c[1] = 1
        self.session.prepare_transaction(
                'prepare_timestamp=' + self.timestamp_str(5))
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(5))
        self.session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(5))
        self.session.commit_transaction()

if __name__ == '__main__':
    wttest.run()
