#!/usr/bin/env python
#
# Public Domain 2014-2020 MongoDB, Inc.
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
from wiredtiger import stat, WT_NOTFOUND

# test_prepare_hs05.py
# Test that after aborting prepare transaction, correct update from the history store is restored.
class test_prepare_hs05(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB'

    format_values = [
        ('column', dict(key_format='r', key=1, value_format='S')),
        ('column-fix', dict(key_format='r', key=1, value_format='8t')),
        ('string-row', dict(key_format='S', key=str(1), value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def test_check_prepare_abort_hs_restore(self):
        uri = 'table:test_prepare_hs05'
        create_params = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, create_params)

        if self.value_format == '8t':
            value1 = 97
            value2 = 98
            value3 = 99
        else:
            value1 = 'a' * 5
            value2 = 'b' * 5
            value3 = 'c' * 5

        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        cursor = self.session.open_cursor(uri)

        key = self.key

        self.session.begin_transaction()
        cursor[key] = value1
        cursor.set_key(key)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

        # Commit update and remove operation in the same transaction.
        self.session.begin_transaction()
        cursor[key] = value2
        cursor.set_key(key)
        cursor.remove()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(3))

        # Add a prepared update for the key.
        self.session.begin_transaction()
        cursor[key] = value3
        self.session.prepare_transaction('prepare_timestamp='+ self.timestamp_str(4))

        # Try to evict the page with prepared update. This will ensure that prepared update is
        # written as the on-disk version and the older versions are moved to the history store.
        session2 = self.conn.open_session()
        session2.begin_transaction('ignore_prepare=true')
        cursor2 = session2.open_cursor(uri, None, "debug=(release_evict=true)")
        cursor2.set_key(key)
        if self.value_format == '8t':
            # In FLCS, deleted values read back as 0.
            self.assertEquals(cursor2.search(), 0)
            self.assertEquals(cursor2.get_value(), 0)
        else:
            self.assertEquals(cursor2.search(), WT_NOTFOUND)
        cursor2.reset()

        # This should abort the prepared transaction.
        self.session.rollback_transaction()

        self.session.checkpoint()

        # We should be able to read the older version of the key from the history store.
        self.session.begin_transaction('read_timestamp='+self.timestamp_str(2))
        cursor.set_key(key)
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), value1)
        self.session.rollback_transaction()

        # The latest version should be marked deleted.
        self.session.begin_transaction()
        cursor.set_key(key)
        if self.value_format == '8t':
            # In FLCS, deleted values read back as 0.
            self.assertEquals(cursor.search(), 0)
            self.assertEquals(cursor.get_value(), 0)
        else:
            self.assertEqual(cursor.search(), WT_NOTFOUND)
        self.session.rollback_transaction()
