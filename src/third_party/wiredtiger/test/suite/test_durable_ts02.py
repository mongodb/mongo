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

from helper import copy_wiredtiger_home
import wiredtiger, wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_durable_ts03.py
#    Checking visibility and durability of updates with durable_timestamp
class test_durable_ts03(wttest.WiredTigerTestCase):
    session_config = 'isolation=snapshot'

    format_values = [
        ('row-string', dict(keyfmt='S', valfmt='S')),
        ('row-int', dict(keyfmt='i', valfmt='S')),
        ('column', dict(keyfmt='r', valfmt='S')),
        ('column-fix', dict(keyfmt='r', valfmt='8t')),
    ]
    types = [
        ('file', dict(uri='file', ds=SimpleDataSet)),
        ('lsm', dict(uri='lsm', ds=SimpleDataSet)),
        ('table-simple', dict(uri='table', ds=SimpleDataSet)),
    ]

    iso_types = [
        ('isolation_read_committed', dict(isolation='read-committed')),
        ('isolation_default', dict(isolation='')),
        ('isolation_snapshot', dict(isolation='snapshot'))
    ]

    def keep(name, d):
        return d['keyfmt'] != 'r' or (d['uri'] != 'lsm' and not d['ds'].is_lsm())

    scenarios = make_scenarios(types, format_values, iso_types, include=keep)

    # Test durable timestamp.
    def test_durable_ts03(self):

        # Build an object.
        uri = self.uri + ':test_durable_ts03'
        ds = self.ds(self, uri, 50, key_format=self.keyfmt, value_format=self.valfmt)
        ds.populate()

        session = self.conn.open_session(self.session_config)
        cursor = session.open_cursor(uri, None)

        # Set stable timestamp to checkpoint initial data set.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(100))
        self.session.checkpoint()

        '''
        Commented out for now: the system panics if we fail after preparing a transaction.

        # Scenario: 1
        # Check to see commit timestamp > durable timestamap, returns error.
        session.begin_transaction()
        self.assertEquals(cursor.next(), 0)
        for i in range(1, 10):
            cursor.set_value(ds.value(111))
            self.assertEquals(cursor.update(), 0)
            self.assertEquals(cursor.next(), 0)

        session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(150))
        msg = "/is less than the commit timestamp/"
        # Check for error when commit timestamp > durable timestamp.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: session.commit_transaction('commit_timestamp=' +\
            self.timestamp_str(200) + ',durable_timestamp=' + self.timestamp_str(180)), msg)

        # Set a stable timestamp so that first update value is durable.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(250))

        # Scenario: 2
        # Check to see durable timestamp < stable timestamp, returns error.
        # Update all values with value 222 i.e. second update value.
        self.assertEquals(cursor.reset(), 0)
        session.begin_transaction()
        self.assertEquals(cursor.next(), 0)
        for i in range(1, 10):
            cursor.set_value(ds.value(222))
            self.assertEquals(cursor.update(), 0)
            self.assertEquals(cursor.next(), 0)

        session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(150))

        msg = "/is less than the stable timestamp/"
        # Check that error is returned when durable timestamp < stable timestamp.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: session.commit_transaction('commit_timestamp=' +\
            self.timestamp_str(200) + ',durable_timestamp=' + self.timestamp_str(240)), msg)
        '''

if __name__ == '__main__':
    wttest.run()
