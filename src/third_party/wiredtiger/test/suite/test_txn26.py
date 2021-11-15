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
# transactions:timestamps
# [END_TAGS]

import wiredtiger, wttest
from wtscenario import make_scenarios

# test_txn26.py
#   Test that commit should fail if commit timestamp is smaller or equal to the active timestamp.
#   Our handling of out of order timestamp relies on this to ensure repeated reads are working as
#   expected.
class test_txn26(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB'
    session_config = 'isolation=snapshot'

    format_values = [
        ('string-row', dict(key_format='S', value_format='S', key=str(0))),
        ('column', dict(key_format='r', value_format='S', key=16)),
        ('column-fix', dict(key_format='r', value_format='8t', key=16)),
    ]
    scenarios = make_scenarios(format_values)

    def test_commit_larger_than_active_timestamp(self):
        if not wiredtiger.diagnostic_build():
            self.skipTest('requires a diagnostic build')

        uri = 'table:test_txn26'
        config = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, config)
        cursor = self.session.open_cursor(uri)
        self.conn.set_timestamp(
            'oldest_timestamp=' + self.timestamp_str(1) + ',stable_timestamp=' + self.timestamp_str(1))

        value = 97 if self.value_format == '8t' else 'a'

        # Start a session with timestamp 10
        session2 = self.conn.open_session(self.session_config)
        session2.begin_transaction('read_timestamp=' + self.timestamp_str(10))

        # Try to commit at timestamp 10
        self.session.begin_transaction()
        cursor[self.key] = value
        with self.expectedStderrPattern("must be greater than the latest active read timestamp"):
            try:
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
            except wiredtiger.WiredTigerError as e:
                gotException = True
                self.pr('got expected exception: ' + str(e))
                self.assertTrue(str(e).find('nvalid argument') >= 0)
        self.assertTrue(gotException, msg = 'expected exception')
