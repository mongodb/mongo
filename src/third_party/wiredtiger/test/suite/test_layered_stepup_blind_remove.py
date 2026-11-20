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

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages

# Verify step-up replays a follower truncate and skips a later orphan blind
# delete.

@disagg_test_class
class test_layered_stepup_blind_remove(wttest.WiredTigerTestCase):
    uri = f"layered:{__qualname__}"
    scenarios = gen_disagg_storages(disagg_only=True)
    conn_config = 'disaggregated=(role="follower")'

    def test_follower_truncate_and_missing_remove_survive_stepup(self):
        self.session.create(self.uri, "key_format=S,value_format=S")

        # Start with two keys.
        with self.transaction(commit_timestamp=1):
            with wttest.open_cursor(self.session, self.uri) as cursor:
                cursor["a"] = "value"
                cursor["b"] = "value"

        # Truncate both of them on the ingest.
        with self.transaction(commit_timestamp=2):
            with (
                wttest.open_cursor(self.session, self.uri) as start_cursor,
                wttest.open_cursor(self.session, self.uri) as stop_cursor,
            ):
                start_cursor.set_key("a")
                stop_cursor.set_key("b")
                self.session.truncate(None, start_cursor, stop_cursor, None)

        # Blind-delete a non-existent key.
        with self.transaction(commit_timestamp=3):
            with wttest.open_cursor(
                self.session, self.uri, config="blind_remove=true"
            ) as cursor:
                cursor.set_key("c")
                self.assertEqual(cursor.remove(), 0)

        # Step up to leader and checkpoint.
        self.assertEqual(
            self.conn.reconfigure('disaggregated=(role="leader")'), 0
        )
        self.conn.set_timestamp("stable_timestamp=" + self.timestamp_str(3))
        self.skipStdoutLinesWithPattern("step-up drain skipped orphan removes")
        self.assertEqual(self.session.checkpoint(), 0)

        # The table is empty.
        with wttest.open_cursor(self.session, self.uri) as cursor:
            self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)

        # The missing-key blind delete was skipped as an orphan.
        orphan_removes = self.get_stat(
            wiredtiger.stat.conn.disagg_step_up_ingest_drain_orphan_remove
        )
        self.assertEqual(orphan_removes, 1)
