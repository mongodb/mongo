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
# test_timestamp11.py
#   Timestamps: mixed timestamp usage
#

from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wtscenario import make_scenarios

class test_timestamp11(wttest.WiredTigerTestCase, suite_subprocess):

    format_values = [
        ('string-row', dict(key_format='S', value_format='S')),
        ('column', dict(key_format='r', value_format='S')),
        ('column-fix', dict(key_format='r', value_format='8t')),
    ]
    scenarios = make_scenarios(format_values)

    def test_timestamp_range(self):
        base = 'timestamp11'
        uri = 'file:' + base
        format = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, format)

        if self.key_format == 'r':
            key = 1
            key2 = 2
        else:
            key = 'key'
            key2 = 'key2'

        if self.value_format == '8t':
            value2 = 200
            value5 = 50
            valueNOTS = 111
        else:
            value2 = 'value2'
            value5 = 'value5'
            valueNOTS = 'valueNOTS'

        # Test that mixed timestamp usage where some transactions use timestamps
        # and others don't behave in the expected way.

        # Insert two data items at timestamp 2
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        self.session.timestamp_transaction(
            'commit_timestamp=' + self.timestamp_str(2))
        c[key] = value2
        c[key2] = value2
        self.session.commit_transaction()
        c.close()

        #
        # Modify one key without a timestamp and modify the other with a
        # later timestamp.
        #
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        self.session.timestamp_transaction(
            'commit_timestamp=' + self.timestamp_str(5))
        c[key] = value5
        self.session.commit_transaction()
        c.close()

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key2] = valueNOTS
        self.session.commit_transaction()
        c.close()

        #
        # Set the stable timestamp and then roll back to it. The first key
        # should roll back to the original value and the second key should
        # remain at the non-timestamped value. Also the non-timestamped value
        # stays regardless of rollbacks or reading at a timestamp.
        #
        stable_ts = self.timestamp_str(2)
        self.conn.set_timestamp('stable_timestamp=' + stable_ts)
        self.session.checkpoint()
        self.conn.rollback_to_stable()

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        self.assertEquals(c[key], value2)
        self.assertEquals(c[key2], valueNOTS)
        self.session.commit_transaction()
        c.close()

        c = self.session.open_cursor(uri)
        self.session.begin_transaction('read_timestamp=' + stable_ts)
        self.assertEquals(c[key], value2)
        self.assertEquals(c[key2], valueNOTS)
        self.session.commit_transaction()
        c.close()

        #
        # Repeat but swapping the keys using or not using timestamps.
        #
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        self.session.timestamp_transaction(
            'commit_timestamp=' + self.timestamp_str(5))
        c[key2] = value5
        self.session.commit_transaction()
        c.close()

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key] = valueNOTS
        self.session.commit_transaction()
        c.close()

        # Read with each timestamp and without any timestamp.
        #
        # Without a timestamp. We should see the latest value for each.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        self.assertEquals(c[key], valueNOTS)
        self.assertEquals(c[key2], value5)
        self.session.commit_transaction()
        c.close()

        # With timestamp 2. Both non-timestamped values override the original
        # value at timestamp 2.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction('read_timestamp=' + stable_ts)
        self.assertEquals(c[key], valueNOTS)
        self.assertEquals(c[key2], valueNOTS)
        self.session.commit_transaction()
        c.close()

        # With timestamp 5. We rolled back the first one and never re-inserted
        # one at that timestamp and inserted without a timestamp. For the second
        # we inserted at timestamp 5 after the non-timestamped insert.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(5))
        self.assertEquals(c[key], valueNOTS)
        self.assertEquals(c[key2], value5)
        self.session.commit_transaction()
        c.close()

if __name__ == '__main__':
    wttest.run()
