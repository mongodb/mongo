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
# test_prepare_conflict.py
#   Evict a page while in the fast-truncate state.
#

import wiredtiger, wttest
from wtdataset import simple_key, simple_value
from wtscenario import make_scenarios

class test_prepare_conflict(wttest.WiredTigerTestCase):
    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('integer_row', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def test_prepare(self):
        # Create a large table with lots of pages.
        uri = "table:test_prepare_conflict"
        format = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, 'allocation_size=512,leaf_page_max=512,' + format)

        if self.value_format == '8t':
            replacement_value = 199
        else:
            replacement_value = "replacement_value"

        cursor = self.session.open_cursor(uri)
        for i in range(1, 80000):
            cursor[simple_key(cursor, i)] = simple_value(cursor, i)
        cursor.close()

        # Force to disk.
        self.reopen_conn()

        # Start a transaction.
        self.session.begin_transaction('isolation=snapshot')

        # Truncate the middle chunk.
        c1 = self.session.open_cursor(uri, None)
        c1.set_key(simple_key(c1, 10000))
        c2 = self.session.open_cursor(uri, None)
        c2.set_key(simple_key(c1, 70000))
        self.session.truncate(None, c1, c2, None)
        c1.close()
        c2.close()

        # Modify a record on a fast-truncate page.
        cursor = self.session.open_cursor(uri)
        cursor[simple_key(cursor, 40000)] = replacement_value
        cursor.close()

        # Prepare and commit the transaction.
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(10))
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(20))
        self.session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(20))
        self.session.commit_transaction()

        # WT-6325 reports WT_PREPARE_CONFLICT while iterating the cursor.
        # Walk the table, the bug will cause a prepared conflict return.
        cursor = self.session.open_cursor(uri)
        while cursor.next() == 0:
            continue
        cursor.close()

if __name__ == '__main__':
    wttest.run()
