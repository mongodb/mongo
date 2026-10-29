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

from helper_disagg import (
    DisaggSchemaEpochMixin,
    disagg_test_class,
    gen_disagg_storages,
)

from wtscenario import make_scenarios

@disagg_test_class
class test_layered_cursor29(wttest.WiredTigerTestCase, DisaggSchemaEpochMixin):
    uri = f"layered:{__qualname__}"
    conn_config = 'disaggregated=(role="leader")'
    scenarios = make_scenarios(gen_disagg_storages(disagg_only=True))

    def create_table_awaiting_publication(self):
        """
        Create a table that remains unpublished until a covering checkpoint.
        """
        self.set_stable_epoch(10)
        self.session.create(self.uri, "key_format=i,value_format=S")
        self.publish(self.uri, 20)

    def test_version_cursor_across_publish(self):
        """
        A version cursor opened before a table's publication can still read a
        key's full history after publication.
        """
        self.create_table_awaiting_publication()

        # Write to the same key multiple times, thereby creating a history.
        key = 1
        with wttest.open_cursor(self.session, self.uri) as cursor:
            for value, ts in [("old", 100), ("new", 200)]:
                with self.transaction(commit_timestamp=ts):
                    cursor[key] = value

        # Open a version cursor before the table is published.
        stable_uri = self.stable_uri(self.uri)
        version_cursor = self.session.open_cursor(
            stable_uri, None, "debug=(dump_version=(enabled=true))"
        )

        # Publish the table with a checkpoint.
        self.set_stable_epoch(20)
        self.leader_checkpoint(300)

        # Evict the page so the old value moves to the history store.
        with wttest.open_cursor(
            self.session, stable_uri, config="debug=(release_evict)"
        ) as evict_cursor:
            evict_cursor.set_key(key)
            evict_cursor.search()

        # After publication, the version cursor should still see the full
        # history.
        with self.transaction(rollback=True):
            LOCATION_INDEX, VALUE_INDEX = 13, 14
            DISK_IMAGE, HISTORY_STORE = 1, 2

            version_cursor.set_key(key)

            # Latest value, from the table's disk image.
            self.assertEqual(version_cursor.search(), 0)
            latest = version_cursor.get_values()
            self.assertEqual(latest[VALUE_INDEX], "new")
            self.assertEqual(latest[LOCATION_INDEX], DISK_IMAGE)

            # Old value, from the history store.
            self.assertEqual(version_cursor.next(), 0)
            older = version_cursor.get_values()
            self.assertEqual(older[VALUE_INDEX], "old")
            self.assertEqual(older[LOCATION_INDEX], HISTORY_STORE)

        version_cursor.close()

    def test_show_prepared_rollback_unpublished(self):
        """An unpublished stable btree rejects show_prepared_rollback."""
        self.create_table_awaiting_publication()

        stable_uri = self.stable_uri(self.uri)
        config = (
            "debug=(dump_version=(enabled=true,show_prepared_rollback=true))"
        )

        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(stable_uri, None, config),
            "/show_prepared_rollback is only supported for in-memory b-trees/",
        )
