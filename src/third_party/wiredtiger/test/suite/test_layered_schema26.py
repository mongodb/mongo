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

# Eviction should be disabled for a table awaiting publication. Apply cache
# pressure before publication and verify no page of the table is evicted.

import wiredtiger
import wttest
from helper_disagg import DisaggSchemaEpochMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_schema26(wttest.WiredTigerTestCase, DisaggSchemaEpochMixin):
    conn_config = (
        "cache_size=10MB,eviction_dirty_target=1,"
        'disaggregated=(role="leader")'
    )

    uri = f"layered:{__qualname__}"
    nrows = 300

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def test_eviction_skips_unpublished_table(self):
        # Create a table but leave it unpublished until the checkpoint under test.
        self.set_stable_epoch(1)
        self.conn.set_timestamp(
            f"oldest_timestamp={self.timestamp_str(1)},"
            f"stable_timestamp={self.timestamp_str(10)}"
        )
        self.session.create(self.uri, "key_format=i,value_format=S")
        self.publish(self.uri, 20)

        # Create enough dirty data to trigger eviction while the table is unpublished.
        eviction_disabled_skips = self.get_stat(
            wiredtiger.stat.conn.eviction_server_skip_trees_eviction_disabled)
        with self.transaction(commit_timestamp=30):
            with wttest.open_cursor(self.session, self.uri) as cursor:
                for i in range(1, self.nrows + 1):
                    cursor[i] = "v" * 2048
        self.conn.set_timestamp(f"stable_timestamp={self.timestamp_str(30)}")

        # The eviction server should have walked the tree and skipped it.
        self.assertStatGreaterSoon(
            wiredtiger.stat.conn.eviction_server_skip_trees_eviction_disabled,
            eviction_disabled_skips,
            timeout=10,
        )

        # No page of the table should have been seen by eviction.
        pages_seen = self.get_stat(
            wiredtiger.stat.dsrc.cache_eviction_pages_seen,
            self.stable_uri(self.uri),
        )
        self.assertEqual(pages_seen, 0)

        # Publish the table through the checkpoint under test.
        self.set_stable_epoch(20)
        self.session.checkpoint()

        # The stable checkpoint should contain every row written before publication.
        with wttest.open_cursor(
            self.session,
            self.stable_uri(self.uri),
            config="checkpoint=WiredTigerCheckpoint",
        ) as cursor:
            count = sum(1 for _ in cursor)
        self.assertEqual(count, self.nrows)
