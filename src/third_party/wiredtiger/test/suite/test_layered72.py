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

import threading, time, wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered72.py
#    Test reading the pinned history store on standby.
@disagg_test_class
class test_layered72(wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),' \
                     + 'precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    create_session_config = 'key_format=S,value_format=S'

    uri = "layered:test_layered72"
    disagg_storages = gen_disagg_storages('test_layered72', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    def test_layered72(self):
        # Create the follower
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' +self.conn_base_config + 'disaggregated=(role="follower")')
        session_follow = conn_follow.open_session('')

        # Create a table
        self.session.create(self.uri, self.create_session_config)

        # Insert a value with timestamp 2
        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction()
        cursor["1"] = "value1"
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(2)}')

        # Update the value with timestamp 3
        self.session.begin_transaction()
        cursor["2"] = "value2"
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(3)}')

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(3)}, oldest_timestamp={self.timestamp_str(1)}')
        self.session.checkpoint()

        # Advance to the latest checkpoint
        self.disagg_advance_checkpoint(conn_follow)

        cursor_follow = session_follow.open_cursor(self.uri, None, None)
        session_follow.begin_transaction(f'read_timestamp={self.timestamp_str(2)}')
        self.assertEqual(cursor_follow["1"], "value1")

        # Update the value with timestamp 4
        self.session.begin_transaction()
        cursor["2"] = "value3"
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(4)}')

        # Make the first version obsolete
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(4)}, oldest_timestamp={self.timestamp_str(3)}')
        self.session.checkpoint()

        # Advance to the latest checkpoint
        self.disagg_advance_checkpoint(conn_follow)

        # Verify we can read the correct value from the history store
        cursor_follow.reset()
        # Cursor should still see the old value at timestamp 2 as it has the history store dhandle pinned
        self.assertEqual(cursor_follow["1"], "value1")
        session_follow.rollback_transaction()
