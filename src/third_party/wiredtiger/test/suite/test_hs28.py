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
from wtscenario import make_scenarios

# test_hs28.py
# Test that we insert a full update instead of a reverse modify to the
# history store if a modify follows a squashed on page value.

class test_hs28(wttest.WiredTigerTestCase):
    conn_config = ''

    key_format_values = [
        ('column', dict(key_format='r')),
        ('row_integer', dict(key_format='i')),
    ]

    scenarios = make_scenarios(key_format_values)

    def conn_config(self):
        config = 'cache_size=50MB,statistics=(all),statistics_log=(json,on_close,wait=1)'
        return config

    def test_insert_hs_full_update(self):
        uri = 'table:test_hs28'
        self.session.create(uri, 'key_format={},value_format=S'.format(self.key_format))

        value1 = "a"
        value2 = "b"

        cursor = self.session.open_cursor(uri)
        # Insert a full value
        self.session.begin_transaction()
        cursor[1] = value1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

        # Do a modify update
        self.session.begin_transaction()
        cursor.set_key(1)
        mods = [wiredtiger.Modify('A', 0, 1)]
        self.assertEqual(cursor.modify(mods), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(5))

        # Commit a transaction with multiple updates on the same key
        self.session.begin_transaction()
        cursor[1] = value1
        cursor[1] = value2
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        # Move the updates to the history store
        self.session.checkpoint()

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        hs_full_update = stat_cursor[wiredtiger.stat.conn.cache_hs_insert_full_update][2]
        hs_reverse_modify = stat_cursor[wiredtiger.stat.conn.cache_hs_insert_reverse_modify][2]
        stat_cursor.close()

        self.assertEqual(hs_full_update, 2)
        self.assertEqual(hs_reverse_modify, 0)
