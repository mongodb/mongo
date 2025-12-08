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
from prepare_util import test_prepare_preserve_prepare_base
from wtscenario import make_scenarios

# Test that prepared transactions are properly handled during page eviction and checkpointing.
# Verify that pages with prepared tombstones are not incorrectly skipped during walks when opening a cursor

class test_prepare43(test_prepare_preserve_prepare_base):
    uri = 'table:test_prepare43'

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('row', dict(key_format='r', value_format='S')),
    ]
    ckpt_precision = [
        ('fuzzy', dict(ckpt_config='precise_checkpoint=false')),
        ('precise', dict(ckpt_config='precise_checkpoint=true,preserve_prepared=true')),
    ]
    scenarios = make_scenarios(format_values, ckpt_precision)

    def test_prepare43(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))

        create_params = 'key_format=' + self.key_format + ',value_format=' + self.value_format
        self.session.create(self.uri, create_params)

        # Insert a value and commit for keys 1-19
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 100):
            value = "commit_value"
            cursor[i] = value
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(21))

        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 100):
            cursor.set_key(i)
            cursor.remove()
        self.session.prepare_transaction(f"prepare_timestamp={self.timestamp_str(25)},prepared_id={self.prepared_id_str(123)}")
        # move the stable ts to be past prepare ts
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(26))

        session2 = self.conn.open_session()
        session2.checkpoint()

        # Force the page to be evicted, checkpoint will write the tombstone as prepared
        session_evict = self.conn.open_session("debug=(release_evict_page=true)")
        session_evict.begin_transaction("ignore_prepare=true")
        evict_cursor = session_evict.open_cursor(self.uri, None, None)
        for i in range(1, 100):
            evict_cursor.set_key(i)
            evict_cursor.search()
            evict_cursor.reset()
        session_evict.rollback_transaction()

        # Check that we can open a checkpoint cursor and find all keys
        cursor = session2.open_cursor(
            self.uri, None, "checkpoint=WiredTigerCheckpoint")
        i = 1
        while True:
            ret = cursor.next()
            if ret != 0:
                break
            self.assertEqual(cursor.get_key(), i)
            self.assertEqual(cursor.get_value(), "commit_value")
            i += 1
        self.assertEqual(i, 100)
