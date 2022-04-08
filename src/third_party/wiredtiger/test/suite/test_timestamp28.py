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
# test_timestamp28.py
#   Timestamps: smoke test that commit is tested at both commit and set time.

import wiredtiger, wttest
from wtdataset import SimpleDataSet

# Timestamps: smoke test that commit is tested at both commit and set time.
class test_timestamp28(wttest.WiredTigerTestCase):
    def test_timestamp28(self):
        uri = 'table:timestamp28'
        ds = SimpleDataSet(self, uri, 50, key_format='i', value_format='S')
        ds.populate()
        c = self.session.open_cursor(uri)

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        self.session.begin_transaction()
        c[5] = 'xxx'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(20)), '/must be after/')

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))
        self.session.begin_transaction()
        c[5] = 'xxx'
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(60))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.commit_transaction(), '/must be after/')

if __name__ == '__main__':
    wttest.run()
