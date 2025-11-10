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

import random
import string
import wttest
from wiredtiger import stat
from wtscenario import make_scenarios
from wtdataset import SimpleDataSet

# test_truncate29.py
# Test that verify handles timestamp usage checks correctly after timestamped fast truncate operations were enabled.

class test_truncate29(wttest.WiredTigerTestCase):
    uri = 'file:test_truncate29'
    conn_config = 'statistics=(all)'
    nrows = 10000

    def generate_random_string(self, length):
        characters = string.ascii_letters + string.digits
        random_string = ''.join(random.choices(characters, k=length))
        return random_string

    def get_fast_truncated_pages(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        pages = stat_cursor[stat.conn.rec_page_delete_fast][2]
        stat_cursor.close()
        return pages

    def test_truncate29(self):
        ds = SimpleDataSet(self, self.uri, 0, key_format='i', value_format='S')
        ds.populate()
        val1 = self.generate_random_string(12345)
        val2 = self.generate_random_string(12345)

        # Insert a large amount of data.
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows):
            self.session.begin_transaction()
            cursor[ds.key(i)] = str(val1)
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(30)}')

        # Insert some more data at a later timestamp.
        for i in range(1, self.nrows):
            self.session.begin_transaction()
            cursor[ds.key(i)] = str(val2)
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(50)}')

        # Make the data globally visible.
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(50)},oldest_timestamp={self.timestamp_str(50)}')

        self.reopen_conn()

        # Have a long-running transaction. Don't commit or roll this back.
        s1 = self.conn.open_session()
        s1.begin_transaction()
        pinned_cursor = s1.open_cursor(self.uri, None)
        pinned_cursor.set_key(ds.key(100))
        pinned_cursor.search()

        # Truncate everything.
        self.session.begin_transaction('no_timestamp=true')
        self.session.truncate(self.uri, None, None, None)
        self.session.commit_transaction()

        fast_truncates_pages = self.get_fast_truncated_pages()
        self.assertGreater(fast_truncates_pages, 0)

        # Do a checkpoint.
        self.session.checkpoint()

        pinned_cursor.close()
        s1.rollback_transaction()

        self.verifyUntilSuccess()
