#!/usr/bin/env python3
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
# Evicting a page after rolling back a prepared transaction that reserved
# and then deleted a key must succeed when preserve_prepared is configured.

import wttest

class test_prepare49(wttest.WiredTigerTestCase):

    test_name = __qualname__
    conn_config = 'precise_checkpoint=true,preserve_prepared=true'
    uri = f'table:{test_name}'

    def _force_evict(self, key, read_ts):
        session = self.conn.open_session()
        try:
            cursor = session.open_cursor(self.uri, None, 'debug=(release_evict)')
            session.begin_transaction(
                'ignore_prepare=true,read_timestamp=' + self.timestamp_str(read_ts))
            cursor.set_key(key)
            self.assertEqual(cursor.search(), 0)
            cursor.reset()
            cursor.close()
            session.rollback_transaction()
        finally:
            session.close()

    def test_evict_after_rollback_with_reserve_between_prepared_ops(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')

        # Commit a stable base value for the key.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(4) +
                                ',oldest_timestamp=' + self.timestamp_str(4))
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor[1] = 'base'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(5))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(5))

        # Prepare a transaction that updates, reserves, then deletes the same
        # key.  The reservation is between the update and the delete, placing
        # all three operations under the same prepared transaction.
        self.session.begin_transaction()
        cursor[1] = 'updated'
        cursor.set_key(1)
        cursor.reserve()
        cursor.set_key(1)
        cursor.remove()
        self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(10) +
            ',prepared_id=' + self.prepared_id_str(1))

        # Advance stable past the prepare timestamp so eviction considers the
        # prepared cell stable, then roll back with a rollback timestamp that
        # is still ahead of stable so the rolled-back prepared cell is retained
        # on disk for recovery purposes.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(15))
        self.session.rollback_transaction(
            'rollback_timestamp=' + self.timestamp_str(20))
        cursor.close()

        # Eviction must complete without crashing.  Reading at the base
        # timestamp to make the key visible for the eviction trigger.
        self._force_evict(1, 5)
