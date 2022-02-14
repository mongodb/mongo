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
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_timestamp23.py
#
# delete keys repeatedly at successive timestamps
class test_timestamp23(wttest.WiredTigerTestCase):
    conn_config = ''

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def test_timestamp(self):

        # Create a file that contains active history (content newer than the oldest timestamp).
        table_uri = 'table:timestamp23'
        ds = SimpleDataSet(
            self, table_uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()
        self.session.checkpoint()

        key = 5
        if self.value_format == '8t':
            value_1 = 97
            value_2 = 98
            value_3 = 99
        else:
            value_1 = 'a' * 500
            value_2 = 'b' * 500
            value_3 = 'c' * 500

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        cursor = self.session.open_cursor(ds.uri)

        # Write two values at timestamp 10. We'll muck with the first value
        # and use the second to reference the page for eviction.
        self.session.begin_transaction('read_timestamp=10')
        cursor[key] = value_1
        cursor[key+1] = value_2
        self.session.commit_transaction('commit_timestamp=11')

        # Delete the first value at timestamp 20.
        self.session.begin_transaction('read_timestamp=20')
        cursor.set_key(key)
        cursor.remove()
        self.session.commit_transaction('commit_timestamp=21')

        # Put it back at timestamp 30.
        self.session.begin_transaction('read_timestamp=30')
        cursor[key] = value_3
        self.session.commit_transaction('commit_timestamp=31')

        # Delete it again at timestamp 40.
        self.session.begin_transaction('read_timestamp=40')
        cursor.set_key(key)
        cursor.remove()
        self.session.commit_transaction('commit_timestamp=41')

        # Evict the page using the second key.
        evict_cursor = self.session.open_cursor(ds.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        v = evict_cursor[key+1]
        self.assertEqual(v, value_2)
        self.assertEqual(evict_cursor.reset(), 0)
        self.session.rollback_transaction()

        # Create a separate session and a cursor to read the original value at timestamp 12.
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(ds.uri)
        session2.begin_transaction('read_timestamp=12')
        v = cursor2[key]
        self.assertEqual(v, value_1)

        self.session.breakpoint()

        # Now delete the original value. This _should_ cause WT_ROLLBACK, but with a column
        # store bug seen and fixed in August 2021, it succeeds, and the resulting invalid
        # tombstone will cause reconciliation to assert. (To see this behavior, comment out the
        # self.fail call and let the transaction commit.)
        try:
            cursor2.remove()
            self.fail("Conflicting remove did not fail")
            session2.commit_transaction('commit_timestamp=50')
        except wiredtiger.WiredTigerError as e:
            self.assertTrue(wiredtiger.wiredtiger_strerror(wiredtiger.WT_ROLLBACK) in str(e))

        cursor.close()
        cursor2.close()

if __name__ == '__main__':
    wttest.run()
