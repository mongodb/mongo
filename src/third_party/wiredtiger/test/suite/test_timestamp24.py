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

# test_timestamp24.py
#
# Make sure certain conflicting operations are rejected.

class test_timestamp24(wttest.WiredTigerTestCase):
    conn_config = ''
    session_config = 'isolation=snapshot'

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('integer_row', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def evict(self, uri, session, key, value):
        evict_cursor = session.open_cursor(uri, None, "debug=(release_evict)")
        session.begin_transaction()
        v = evict_cursor[key]
        self.assertEqual(v, value)
        self.assertEqual(evict_cursor.reset(), 0)
        session.rollback_transaction()

    def test_timestamp(self):

        table_uri = 'table:timestamp24'
        ds = SimpleDataSet(
            self, table_uri, 0, key_format=self.key_format, value_format=self.value_format,
            config='log=(enabled=false)')
        ds.populate()
        self.session.checkpoint()

        key = 5
        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
            value_d = 100
        else:
            value_a = 'a' * 500
            value_b = 'b' * 500
            value_c = 'c' * 500
            value_d = 'd' * 500

        # Pin oldest and stable to timestamp 1.
        #self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
        #    ',stable_timestamp=' + self.timestamp_str(1))

        # Create two sessions so we can have two transactions.
        session1 = self.session
        session2 = self.conn.open_session()

        # In session 1, write value_a at time 20.
        # Commit that and then start a new transaction. Read the value back.
        # Then reset the cursor so the page isn't pinned, but leave the transaction open.
        cursor1 = session1.open_cursor(ds.uri)
        session1.begin_transaction()
        cursor1[key] = value_a
        session1.commit_transaction('commit_timestamp=20')
        session1.begin_transaction('read_timestamp=25')
        tmp = cursor1[key]
        self.assertEqual(tmp, value_a)
        cursor1.reset()
        # leave session1's transaction open

        # In session 2, write value_b at time 50. Commit that.
        cursor2 = session2.open_cursor(ds.uri)
        session2.begin_transaction()
        cursor2[key] = value_b
        session2.commit_transaction('commit_timestamp=50')
        cursor2.reset()

        # Evict the page to force reconciliation. value_b goes to disk; value_a to history.
        # Use session2 so we can keep session1's transaction open.
        self.evict(ds.uri, session2, key, value_b)

        # In session 2, write value_c, but abort it.
        session2.begin_transaction()
        cursor2[key] = value_c
        session2.rollback_transaction()

        # Now in session 1 try to write value_d. This should produce WT_ROLLBACK, but with
        # a bug seen and fixed in August 2021, succeeds improperly instead, resulting in
        # data corruption. The behavior is more exciting when the update is a modify (the
        # modify gets applied to value_b instead of value_a, producing a more detectable
        # corruption) but this is not necessary to check the wrong behavior.

        try:
            cursor1[key] = value_d
            self.fail("Conflicting update did not fail")
            broken = True
        except wiredtiger.WiredTigerError as e:
            self.assertTrue(wiredtiger.wiredtiger_strerror(wiredtiger.WT_ROLLBACK) in str(e))
            broken = False

        # Put this outside the try block in case it throws its own exceptions
        if broken:
            session1.commit_transaction('commit_timestamp=30')
        else:
            session1.rollback_transaction()

        # Read the data back
        session2.begin_transaction('read_timestamp=60')
        tmp = cursor2[key]

        # It should be value_b. But if we broke, it'll be value_d.
        self.assertEqual(tmp, value_d if broken else value_b)

        cursor2.close()
        cursor1.close()

if __name__ == '__main__':
    wttest.run()
