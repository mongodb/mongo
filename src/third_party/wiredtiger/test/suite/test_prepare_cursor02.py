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
from wtdataset import SimpleDataSet, SimpleIndexDataSet
from wtdataset import SimpleLSMDataSet, ComplexDataSet, ComplexLSMDataSet
from wtscenario import make_scenarios

# test_prepare_cursor02.py
#    WT_CURSOR navigation (next/prev) tests with prepared transactions
class test_prepare_cursor02(wttest.WiredTigerTestCase):
    session_config = 'isolation=snapshot'

    keyfmt = [
        ('row-store', dict(keyfmt='i', valfmt='S')),
        ('column-store', dict(keyfmt='r', valfmt='S')),
        ('fixed-length-column-store', dict(keyfmt='r', valfmt='8t')),
    ]
    types = [
        ('table-simple', dict(uri='table', ds=SimpleDataSet)),
    ]

    scenarios = make_scenarios(types, keyfmt)

    # Test cursor navigate (next/prev) with prepared transactions.
    def test_cursor_navigate_prepare_transaction(self):

        # Build an object.
        uri = self.uri + ':test_prepare_cursor02'
        ds = self.ds(self, uri, 0, key_format=self.keyfmt, value_format=self.valfmt)
        ds.populate()

        session = self.session
        cursor = session.open_cursor(uri, None)
        session.begin_transaction()
        cursor.set_key(ds.key(1))
        cursor.set_value(ds.value(1))
        cursor.insert()
        session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(100))

        prep_session = self.conn.open_session(self.session_config)
        prep_cursor = prep_session.open_cursor(uri, None)

        # Check cursor navigate with insert in prepared transaction.
        # Data set is empty
        # Insert key 1 in prepared state.
        prep_session.begin_transaction()
        # Check next operation.
        prep_cursor.set_key(ds.key(1))
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: prep_cursor.search())
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: prep_cursor.next())
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: prep_cursor.next())

        # Check prev operation.
        prep_cursor.set_key(ds.key(1))
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: prep_cursor.search())
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: prep_cursor.prev())
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: prep_cursor.prev())
        prep_cursor.close()
        prep_session.commit_transaction()

        session.rollback_transaction()

if __name__ == '__main__':
    wttest.run()
