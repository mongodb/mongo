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
# test_prepare40.py
# Test that checkpoint after opening a backup with prepared updates (preserve prepared on) doesn't crash by:
# - Checkpoint writes prepared update to disk
# - Rollback the transaction with rollback timestamp > stable timestamp
# - Checkpoint again. it should not crash and should write the rolled back update as prepared to disk

import wiredtiger
from prepare_util import test_prepare_preserve_prepare_base
from wtscenario import make_scenarios

class test_prepare40(test_prepare_preserve_prepare_base):
    tablename = 'test_prepare40'
    uri = 'table:' + tablename
    conn_config = 'precise_checkpoint=true,preserve_prepared=true'


    def test_prepare40(self):

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(50))
        create_params = 'key_format=i,value_format=S'
        self.session.create(self.uri, create_params)
        c = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        c[1] = "commit ts=60"
        c[2] = "commit ts=60"
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(60))

        # Insert a few keys then prepare the transaction
        self.session.begin_transaction()
        c[3] = "prepare ts=100"
        c[4] = "prepare ts=100"
        c[5] = "prepare ts=100"
        # Prepare with a timestamp greater than current stable
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(100) +',prepared_id=' + self.prepared_id_str(123))
        # Move the stable timestamp to include the prepared transaction
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(150))
        # Create a checkpoint
        session2 = self.conn.open_session()
        session2.checkpoint()

        # Force eviction to ensures the committed update gets written to disk storage
        # and flush in-memory updates. The aim of this is to hit RESOLVE_PREPARE_ON_DISK case
        # when we rollback the transaction
        session_evict = self.conn.open_session("debug=(release_evict_page=true)")
        session_evict.begin_transaction("ignore_prepare=true")
        evict_cursor = session_evict.open_cursor(self.uri, None, None)
        for i in range(1, 3):  # Evict to trigger reconciliation
            evict_cursor.set_key(i)
            self.assertEqual(evict_cursor.search(), 0)
            evict_cursor.reset()
        evict_cursor.close()
        session_evict.rollback_transaction()
        session_evict.close()

        self.session.rollback_transaction("rollback_timestamp=" + self.timestamp_str(200))

        session3 = self.conn.open_session()
        # Check that we're writing updates as prepared again
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_time_window_prepared: True,
        }, self.uri, session3)
