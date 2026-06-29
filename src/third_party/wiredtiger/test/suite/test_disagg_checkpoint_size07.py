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

import re, wiredtiger, wttest
from wiredtiger import stat
from helper import WiredTigerCursor
from helper_disagg import DisaggConfigMixin, disagg_test_class

# Tests for database size on schema drop.

@disagg_test_class
class test_disagg_checkpoint_size07(wttest.WiredTigerTestCase):
    conn_config = 'disaggregated=(role="leader")'

    # A filler table that is never dropped, giving the database size enough headroom that the
    # (buggy) repeated subtraction does not underflow before we have observed the pattern.
    keep_uri = "layered:keep_table"
    keep_base = "keep_table"

    # The large collection we will repeatedly fail to drop.
    victim_uri = "layered:victim_table"
    victim_base = "victim_table"

    value_size = 8000
    num_failed_ckpts = 4

    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        DisaggConfigMixin.conn_extensions(self, extlist)

    def get_database_size(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        value = stat_cursor[stat.conn.disagg_database_size][2]
        stat_cursor.close()
        return value

    def exists(self, uri):
        meta_cursor = self.session.open_cursor('metadata:')
        meta_cursor.set_key(uri)
        ret = meta_cursor.search()
        meta_cursor.close()
        return ret == 0

    def insert(self, uri, nrows, start=0):
        cursor = self.session.open_cursor(uri)
        for i in range(start, start + nrows):
            cursor[str(i)] = str(i) + 'x' * self.value_size
        cursor.close()

    def database_size_check(self, context_message=""):
        self.session.checkpoint()

        # Calculate reference value from sum of all the collections
        accumulated_database_size = 0
        with WiredTigerCursor(self.session, 'metadata:') as cursor:
            while cursor.next() == 0:
                value = cursor.get_value()
                checkpoints = re.findall(r',checkpoint=([^)]+),', value)
                if checkpoints:
                    sizes = re.findall(r',size=(\d+),', checkpoints[0])
                    if sizes:
                        accumulated_database_size += int(sizes[-1])

        database_size = self.get_database_size()
        self.assertGreaterEqual(database_size, accumulated_database_size,
            f"database size {database_size} should be greater or equal to "
            f"accumulated {accumulated_database_size} : {context_message}")

    def test_failed_drop_does_not_shrink_database_size(self):
        # Filler table for headroom.
        self.session.create(self.keep_uri, 'key_format=S,value_format=S')
        self.insert(self.keep_uri, 6000)

        # The large collection that the drop will keep failing on.
        self.session.create(self.victim_uri, 'key_format=S,value_format=S')
        self.insert(self.victim_uri, 1000)

        self.session.checkpoint()

        # Hold the layered data handle busy from another session, but never use the cursor: the
        # constituent (stable/ingest) cursors stay closed, so only the top-level handle is pinned.
        # The drop's leader trim and constituent drops then succeed, but the final close fails with
        # EBUSY -- after the REMOVE has already been enqueued.
        busy_session = self.conn.open_session()
        busy_cursor = busy_session.open_cursor(self.victim_uri)

        keep_row = 6000
        for i in range(self.num_failed_ckpts):
            # A little new data so the checkpoint has a non-zero size delta; the database size
            # update is gated on that.
            self.insert(self.keep_uri, 20, start=keep_row)
            keep_row += 20

            # The caller always retries the drop while the collection still exists.
            self.assertTrue(
                self.raisesBusy(lambda: self.session.drop(self.victim_uri, None)),
                "a failed drop must raise EBUSY")
            self.assertTrue(self.exists(self.victim_uri),
                "a failed drop must leave the collection in place")
            self.session.checkpoint()

        self.database_size_check("after failed drops")

        # With the cursor gone the retried drop finally succeeds.
        busy_cursor.close()
        busy_session.close()
        self.assertTrue(self.exists(self.victim_uri))

        # We should see the drop size for a successful drop
        with self.expectedStdoutPattern('.*Accumulated drop size.*', maxchars=1000000):
            self.conn.reconfigure("verbose=[disaggregated_storage:2]")
            self.session.drop(self.victim_uri, None)
            self.session.checkpoint()
            self.conn.reconfigure("verbose=[disaggregated_storage:0]")

        self.assertFalse(self.exists(self.victim_uri))
        self.database_size_check("after successful drop")
