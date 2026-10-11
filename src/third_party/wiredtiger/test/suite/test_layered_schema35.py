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
from contextlib import closing

from helper_disagg import (
    DisaggSchemaEpochMixin,
    disagg_test_class,
    gen_disagg_storages,
)

from prepare_util import test_prepare_preserve_prepare_base
from wtscenario import make_scenarios


# Prepared insert rollback preserves state across publication and release eviction.
@disagg_test_class
class test_layered_schema35(test_prepare_preserve_prepare_base, DisaggSchemaEpochMixin):
    uri = f"layered:{__qualname__}"

    conn_config = (
        test_prepare_preserve_prepare_base.conn_config
        + ',disaggregated=(role="leader")'
    )

    disagg_storages = gen_disagg_storages(disagg_only=True)
    release_evict_modes = [
        ("cursor_evict", dict(session_cfg="", cursor_cfg="debug=(release_evict)")),
        ("session_evict", dict(session_cfg="debug=(release_evict_page=true)", cursor_cfg="")),
    ]
    scenarios = make_scenarios(disagg_storages, release_evict_modes)

    def prepare_insert_and_rollback(self, prepare_ts, rollback_ts):
        """Prepare an insert and roll it back."""
        with wttest.open_cursor(self.session, self.uri) as cursor:
            self.session.begin_transaction()
            cursor[1] = "rolled-back"
            self.session.prepare_transaction(
                f"prepare_timestamp={self.timestamp_str(prepare_ts)},"
                f"prepared_id={self.prepared_id_str(1)}"
            )
            self.session.rollback_transaction(
                f"rollback_timestamp={self.timestamp_str(rollback_ts)}"
            )

    def release_evict(self):
        """Search for the rolled-back key, releasing its page with eviction configured."""
        with (
            closing(self.conn.open_session(self.session_cfg)) as session,
            wttest.open_cursor(session, self.uri, config=self.cursor_cfg) as cursor,
        ):
            cursor.set_key(1)
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)

    def evicted_pages(self):
        """Return the number of pages evicted from the stable file."""
        uri = self.stable_uri(self.uri)
        clean = self.get_stat(wiredtiger.stat.dsrc.cache_eviction_clean, uri)
        dirty = self.get_stat(wiredtiger.stat.dsrc.cache_eviction_dirty, uri)
        return clean + dirty

    def prepared_records_written(self):
        """Return the number of records with prepare state written to the stable file."""
        return self.get_stat(
            wiredtiger.stat.dsrc.rec_time_window_prepared, self.stable_uri(self.uri)
        )

    def test_prepared_insert_rollback_before_publication(self):
        # Create a table that is awaiting publication.
        self.set_stable_epoch(5)
        self.session.create(self.uri, "key_format=i,value_format=S")
        self.publish(self.uri, 10)

        # Roll back a prepared insert while the table is awaiting publication.
        self.prepare_insert_and_rollback(prepare_ts=30, rollback_ts=40)

        # Publish the table and checkpoint before the rollback is stable.
        self.set_stable_epoch(10)
        self.leader_checkpoint(30)

        # Confirm the stable file received a record with prepare state.
        self.assertGreater(self.prepared_records_written(), 0)

        # Confirm the rolled-back key is absent and its page is evicted.
        evicted = self.evicted_pages()
        self.release_evict()
        self.assertGreater(self.evicted_pages(), evicted)

        # Make the rollback stable and checkpoint again.
        self.leader_checkpoint(40)

    def test_prepared_insert_rollback_not_evicted_before_publication(self):
        # Create a table that is awaiting publication.
        self.set_stable_epoch(5)
        self.session.create(self.uri, "key_format=i,value_format=S")
        self.publish(self.uri, 10)

        # Roll back a prepared insert while the table is awaiting publication.
        self.prepare_insert_and_rollback(prepare_ts=30, rollback_ts=40)

        # Confirm eviction does not happen before the table is published.
        evicted = self.evicted_pages()
        self.release_evict()
        self.assertEqual(self.evicted_pages(), evicted)

        # Publish the table and checkpoint before the rollback is stable.
        self.set_stable_epoch(10)
        self.leader_checkpoint(30)

        # Confirm the eviction attempt did not discard the prepared state.
        self.assertGreater(self.prepared_records_written(), 0)

        # Make the rollback stable and checkpoint again.
        self.leader_checkpoint(40)


if __name__ == "__main__":
    wttest.run()
