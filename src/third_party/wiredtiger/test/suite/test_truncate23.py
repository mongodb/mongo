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

# test_truncate23.py
# Test that we properly handle truncate with and without prepared transactions.
class test_truncate23(wttest.WiredTigerTestCase):
    uri_prefix = 'table:test_truncate23_'
    conn_config = 'statistics=(all)'
    scenario_num = 0

    def in_range(self, truncate_start, truncate_stop, key):
        '''
        Check whether the key is in the truncation range.
        '''
        if truncate_start is None and truncate_stop is None:
            return True
        elif truncate_start is None:
            return key <= truncate_stop
        elif truncate_stop is None:
            return key >= truncate_start
        else:
            return key >= truncate_start and key <= truncate_stop

    def scenario(self, prepared, truncate_start, truncate_stop, *args):
        '''
        Run through a test scenario, which populates a table with the args and truncates the given
        range. If the prepared argument is true, keys outside of the truncation range are populated
        in a prepared transaction that does not commit until after the truncate.
        '''
        self.scenario_num += 1
        uri = self.uri_prefix + str(self.scenario_num)
        self.session.create(uri, 'key_format=Q,value_format=Q,log=(enabled=false)')

        # Populate the truncation range with data that must be committed.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(uri)
        for key in args:
            if self.in_range(truncate_start, truncate_stop, key):
                cursor.set_key(key)
                cursor.set_value(key)
                cursor.insert()
        cursor.close()
        self.session.commit_transaction('commit_timestamp=10')

        # Populate data outside of the truncation range.
        session2 = self.conn.open_session()
        session2.begin_transaction()
        cursor = session2.open_cursor(uri)
        for key in args:
            if not self.in_range(truncate_start, truncate_stop, key):
                cursor.set_key(key)
                cursor.set_value(key)
                cursor.insert()
        cursor.close()
        if prepared:
            session2.prepare_transaction('prepare_timestamp=20')
        else:
            session2.commit_transaction('commit_timestamp=20')

        # Truncate the range.
        self.session.begin_transaction()
        cursor_start = None
        cursor_stop = None
        if truncate_start is not None:
            cursor_start = self.session.open_cursor(uri)
            cursor_start.set_key(truncate_start)
        if truncate_stop is not None:
            cursor_stop = self.session.open_cursor(uri)
            cursor_stop.set_key(truncate_stop)
        truncate_uri = uri if truncate_start is None and truncate_stop is None else None
        self.session.truncate(truncate_uri, cursor_start, cursor_stop, None)
        if cursor_start is not None:
            cursor_start.close()
        if cursor_stop is not None:
            cursor_stop.close()
        self.session.commit_transaction('commit_timestamp=30')

        # Clean up the extra session.
        if prepared:
            session2.commit_transaction('commit_timestamp=40,durable_timestamp=41')
        session2.close()

        # Gather the list of keys in the table.
        cursor = self.session.open_cursor(uri)
        have = []
        while cursor.next() == 0:
            have.append(cursor.get_key())

        # Compare to what we expect.
        expect = sorted(filter(lambda k: not self.in_range(truncate_start, truncate_stop, k), args))
        self.assertEqual(have, expect)

    def test_truncate23(self):
        '''
        Test boundary conditions for truncate with and without prepared transactions.
        '''

        # This test is disabled until we determine how to handle prepare conflicts with keys adjacent
        # to the truncation range
        self.skipTest("FIXME-WT-13232")

        # First test this without the prepared transactions to ensure that everything works.
        for prepared in [False, True]:

            # The start and stop keys exist (if not None).
            self.scenario(prepared, 1000, 2000, 500, 1000, 1500, 2000, 2500)
            self.scenario(prepared, None, 2000, 500, 1000, 1500, 2000, 2500)
            self.scenario(prepared, 1000, None, 500, 1000, 1500, 2000, 2500)
            self.scenario(prepared, None, None, 500, 1000, 1500, 2000, 2500)

            # The start and stop keys don't exist.
            self.scenario(prepared, 1000, 2000, 500, 999, 1500, 2001, 2500)
            self.scenario(prepared, None, 2000, 500, 999, 1500, 2001, 2500)
            self.scenario(prepared, 1000, None, 500, 999, 1500, 2001, 2500)
            self.scenario(prepared, None, None, 500, 999, 1500, 2001, 2500)

            # Empty transaction range.
            self.scenario(prepared, 1000, 2000, 500, 999, 2001, 2500)
            self.scenario(prepared, None, 2000, 500, 999, 2001, 2500)
            self.scenario(prepared, 1000, None, 500, 999, 2001, 2500)
            self.scenario(prepared, None, None, 500, 999, 2001, 2500)
