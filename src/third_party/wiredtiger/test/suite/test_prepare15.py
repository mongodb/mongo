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

import wttest
from wiredtiger import WT_NOTFOUND
from wtscenario import make_scenarios

# test_prepare15.py
# Test that the prepare transaction rollback removes the on-disk key
# or replace it with history store and commit retains the changes when
# both insert and remove operations are from the same transaction.
class test_prepare15(wttest.WiredTigerTestCase):
    in_memory_values = [
        ('no_inmem', dict(in_memory=False)),
        ('inmem', dict(in_memory=True))
    ]

    key_format_values = [
        ('column', dict(key_format='r')),
        ('integer_row', dict(key_format='i')),
    ]

    txn_end_values = [
        ('commit', dict(commit=True)),
        ('rollback', dict(commit=False)),
    ]

    scenarios = make_scenarios(in_memory_values, key_format_values, txn_end_values)

    def conn_config(self):
        config = 'cache_size=50MB'
        if self.in_memory:
            config += ',in_memory=true'
        else:
            config += ',in_memory=false'
        return config

    def test_prepare_restore_hs_update(self):
        # Create a table without logging.
        uri = "table:prepare15"
        create_config = 'allocation_size=512,key_format=S,value_format=S'
        self.session.create(uri, create_config)

        # Pin oldest and stable timestamps to 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        valuea = 'a'
        valueb = 'a'

        # Perform an update and remove.
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        cursor[str(0)] = valuea
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        self.session.begin_transaction()
        cursor.set_key(str(0))
        cursor.remove()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor.close()

        # Perform an update and remove.
        s = self.conn.open_session()
        cursor = s.open_cursor(uri)
        s.begin_transaction()
        cursor[str(0)] = valueb
        cursor.set_key(str(0))
        cursor.remove()
        cursor.close()
        s.prepare_transaction('prepare_timestamp=' + self.timestamp_str(40))

        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")

        # Search for the key so we position our cursor on the page that we want to evict.
        self.session.begin_transaction('ignore_prepare = true')
        evict_cursor.set_key(str(0))
        self.assertEquals(evict_cursor.search(), WT_NOTFOUND)
        evict_cursor.reset()
        evict_cursor.close()
        self.session.commit_transaction()

        if self.commit:
            # Commit the prepared transaction
            s.timestamp_transaction('commit_timestamp=' + self.timestamp_str(50))
            s.timestamp_transaction('durable_timestamp=' + self.timestamp_str(60))
            s.commit_transaction()
        else:
            # Rollback the prepared transaction
            s.rollback_transaction()

        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")

        # Search for the key so we position our cursor on the page that we want to evict.
        self.session.begin_transaction()
        evict_cursor.set_key(str(0))
        self.assertEquals(evict_cursor.search(), WT_NOTFOUND)
        evict_cursor.reset()
        evict_cursor.close()
        self.session.commit_transaction()

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(20))
        cursor2 = self.session.open_cursor(uri)
        cursor2.set_key(str(0))
        self.assertEquals(cursor2.search(), 0)
        self.assertEqual(cursor2.get_value(), valuea)
        self.session.commit_transaction()

    def test_prepare_not_found(self):
        # Create a table without logging.
        uri = "table:prepare15"
        create_config = 'allocation_size=512,key_format=S,value_format=S'
        self.session.create(uri, create_config)

        # Pin oldest and stable timestamps to 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        value = 'a'

        # Perform an update and remove.
        s = self.conn.open_session()
        cursor = s.open_cursor(uri)
        s.begin_transaction()
        cursor[str(0)] = value
        cursor.set_key(str(0))
        cursor.remove()
        cursor.close()
        s.prepare_transaction('prepare_timestamp=' + self.timestamp_str(20))

        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")

        # Search for the key so we position our cursor on the page that we want to evict.
        self.session.begin_transaction("ignore_prepare = true")
        evict_cursor.set_key(str(0))
        self.assertEquals(evict_cursor.search(), WT_NOTFOUND)
        evict_cursor.reset()
        evict_cursor.close()
        self.session.commit_transaction()

        if self.commit:
            # Commit the prepared transaction
            s.timestamp_transaction('commit_timestamp=' + self.timestamp_str(30))
            s.timestamp_transaction('durable_timestamp=' + self.timestamp_str(40))
            s.commit_transaction()
        else:
            # Rollback the prepared transaction
            s.rollback_transaction()

        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")

        # Search for the key so we position our cursor on the page that we want to evict.
        self.session.begin_transaction()
        evict_cursor.set_key(str(0))
        self.assertEquals(evict_cursor.search(), WT_NOTFOUND)
        evict_cursor.reset()
        evict_cursor.close()
        self.session.commit_transaction()

        self.session.begin_transaction()
        cursor2 = self.session.open_cursor(uri)
        cursor2.set_key(str(0))
        self.assertEquals(cursor2.search(), WT_NOTFOUND)
        self.session.commit_transaction()
