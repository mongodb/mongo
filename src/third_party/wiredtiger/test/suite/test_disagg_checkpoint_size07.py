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

    # The shared metadata table. Its own checkpoint size moves with the drop.
    shared_uri = "file:WiredTigerShared.wt_stable"

    # The fixed overhead every database size carries for the KEK table and shared turtle page.
    WT_DISAGG_CHECKPOINT_SIZE_BUFFER = 1024 * 1024

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

    def get_checkpoint_size(self, uri):
        """The size of a file's most recent checkpoint, as the database size counts it."""
        meta_cursor = self.session.open_cursor('metadata:')
        meta_cursor.set_key(uri)
        self.assertEqual(meta_cursor.search(), 0)
        sizes = re.findall(r',size=(\d+),', meta_cursor.get_value())
        meta_cursor.close()
        self.assertGreater(len(sizes), 0, f"no checkpoint size in the metadata for {uri}")
        return int(sizes[-1])

    def accumulated_stable_size(self):
        """The database size as the metadata defines it: every stable file's last checkpoint."""
        total = 0
        with WiredTigerCursor(self.session, 'metadata:') as cursor:
            while cursor.next() == 0:
                uri = cursor.get_key()
                if not uri.startswith('file:') or not uri.endswith('.wt_stable'):
                    continue
                sizes = re.findall(r',size=(\d+),', cursor.get_value())
                if sizes:
                    total += int(sizes[-1])
        return total

    def database_size_check(self, context_message=""):
        self.session.checkpoint()

        accumulated_database_size = \
            self.accumulated_stable_size() + self.WT_DISAGG_CHECKPOINT_SIZE_BUFFER
        database_size = self.get_database_size()
        self.assertEqual(database_size, accumulated_database_size,
            f"database size {database_size} should equal the metadata's own total "
            f"{accumulated_database_size} : {context_message}")

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

        # A successful drop takes the collection's whole size out of the database size. The same
        # checkpoint applies the drop's removal to the shared metadata table, whose own checkpoint
        # size moves as a result; that part of the change does not belong to the collection.
        victim_size = self.get_checkpoint_size("file:" + self.victim_base + ".wt_stable")
        shared_before_drop = self.get_checkpoint_size(self.shared_uri)
        size_before_drop = self.get_database_size()
        self.session.drop(self.victim_uri, None)
        self.session.checkpoint()
        shared_after_drop = self.get_checkpoint_size(self.shared_uri)
        size_after_drop = self.get_database_size()
        self.assertEqual(size_before_drop - size_after_drop,
            victim_size - (shared_after_drop - shared_before_drop),
            f"a successful drop must remove the collection's size ({victim_size}) from the database "
            f"size: {size_before_drop} -> {size_after_drop}, shared metadata "
            f"{shared_before_drop} -> {shared_after_drop}")

        self.assertFalse(self.exists(self.victim_uri))
        self.database_size_check("after successful drop")
