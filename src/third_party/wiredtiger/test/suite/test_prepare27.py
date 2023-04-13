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
from wtscenario import make_scenarios

# test_prepare27.py
# Test that a prepared update that has been aborted is not selected as the base value.
class test_prepare27(wttest.WiredTigerTestCase):
    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column-fix', dict(key_format='r', value_format='8t')),
        ('integer-row', dict(key_format='i', value_format='S')),
        ('string-row', dict(key_format='S', value_format='S')),
    ]
    scenarios = make_scenarios(format_values)

    def create_key(self, i):
        return str(i) if self.key_format == 'S' else i

    def create_value(self, i):
        return str(i) if self.value_format == 'S' else i

    def evict_cursor(self, uri, session_cfg, nrows):
        s = self.conn.open_session()
        s.begin_transaction(session_cfg)
        evict_cursor = s.open_cursor(uri, None, "debug=(release_evict)")
        for i in range(1, nrows + 1):
            evict_cursor.set_key(self.create_key(i))
            evict_cursor.search()
            evict_cursor.reset()
        s.rollback_transaction()
        evict_cursor.close()

    def test_prepare27(self):
        uri = 'table:test_prepare27'
        create_params = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, create_params)

        # Apply a series of updates to a key.
        num_updates = 5
        key = self.create_key(1)

        cursor = self.session.open_cursor(uri)
        for ts in range(1, num_updates + 1):
            with self.transaction(commit_timestamp = ts):
                value = ts
                cursor[key] = self.create_value(value)
        
        # Set the stable timestamp.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(2))
    
        # At this point we have 5 updates associated with a key in the update chain:
        # 1 -> 2 -> 3 -> 4 -> 5

        # Perform a new update through a prepared transaction and evict the page.
        num_updates = num_updates + 1
        ts = num_updates

        self.session.begin_transaction()
        cursor[key] = self.create_value(ts)
        self.session.prepare_transaction('prepare_timestamp=' + str(ts))

        # Note that we need to configure the session to ignore prepared updates to be able to
        # perform eviction. After eviction, we should have the prepared update on the DS and the
        # rest in the HS.
        self.evict_cursor(uri, "ignore_prepare=true", 1)

        # Roll back the prepared transaction. This will mark the prepared update as aborted and
        # bring back the previous update to the update chain.
        self.session.rollback_transaction()

        # By calling RTS, this should bring the latest stable update (2) to the update chain and
        # leave the stable one (1) in the HS. We should have the following update chain:
        # 2 -> 6 -> 5, with 6 and 5 aborted.
        self.conn.rollback_to_stable()
    
        # Now search for the first record at the time it was committed.
        self.session.begin_transaction('read_timestamp=1')
        cur = self.session.open_cursor(uri, None, None)
        cur.set_key(key)
        cur.search()
