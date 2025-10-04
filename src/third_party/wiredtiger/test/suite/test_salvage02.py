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

# Remove the history store, and check that we can start back up.

import os
import wttest

from wiredtiger import wiredtiger_strerror, WiredTigerError, WT_ROLLBACK
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

class test_salvage02(wttest.WiredTigerTestCase):
    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    uri = "table:salvage02"

    def large_updates(self, value, ds, nrows, commit_ts):
        # Update a large number of records.
        session = self.session
        try:
            cursor = session.open_cursor(self.uri)
            for i in range(1, nrows + 1):
                if commit_ts == 0:
                    session.begin_transaction('no_timestamp=true')
                else:
                    session.begin_transaction()
                cursor[ds.key(i)] = value
                if commit_ts == 0:
                    session.commit_transaction()
                else:
                    session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
            cursor.close()
        except WiredTigerError as e:
            rollback_str = wiredtiger_strerror(WT_ROLLBACK)
            if rollback_str in str(e):
                session.rollback_transaction()
            raise(e)

    def test_hs_removed(self):
        nrows = 1000

        if self.value_format == '8t':
            valuea = 97
            valueb = 98
        else:
            valuea = "aaaaa" * 100
            valueb = "bbbbb" * 100

        # Start normally, insert data
        ds = SimpleDataSet(self, self.uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()
        self.large_updates(valuea, ds, nrows, 1)

        # Put some content in the history store
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))
        ds2 = SimpleDataSet(self, self.uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds2.populate()
        self.large_updates(valueb, ds2, nrows, 2)

        self.session.checkpoint()

        # Restart with salvage configured
        self.close_conn()
        os.remove('WiredTigerHS.wt')
        self.open_conn(config='salvage=true')
        results = list(self.session.open_cursor(self.uri))
        assert(len(results) == nrows)
