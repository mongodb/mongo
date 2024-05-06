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
# compression
# [END_TAGS]
#
# test_dictionary03.py
#       Test cells with same values are reused through the dictionary despite time window
#       information.

from wtscenario import make_scenarios
from wtdataset import simple_key
from wiredtiger import stat
import wttest

class test_dictionary03(wttest.WiredTigerTestCase):
    scenarios = make_scenarios([
        ('row', dict(key_format='S')),
        ('var', dict(key_format='r')),
    ])

    value_a = "aaa" * 100
    value_b = "bbb" * 100

    def test_dictionary03(self):
        uri = 'file:test_dictionary03'

        # Use a reasonably large page size so all of the items fit on a page.
        config=f'leaf_page_max=64K,dictionary=100,value_format=S,key_format={self.key_format}'
        self.session.create(uri, config)
        cursor = self.session.open_cursor(uri, None)

        # Pin timestamps to ensure new modifications are not globally visible.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Write two values that will lead to two unique dictionary entries.
        cursor[simple_key(cursor, 1)] = self.value_a
        cursor[simple_key(cursor, 2)] = self.value_b

        # The next cells will reuse an existing dictionary entry and have time window validity.
        self.session.begin_transaction()
        cursor[simple_key(cursor, 3)] = self.value_a
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        cursor.close()

        # Checkpoint to force the pages through reconciliation.
        self.session.checkpoint()

        # Confirm the dictionary was effective.
        cursor = self.session.open_cursor('statistics:' + uri, None, None)
        dict_value = cursor[stat.dsrc.rec_dictionary][2]
        self.assertEqual(dict_value, 1)
