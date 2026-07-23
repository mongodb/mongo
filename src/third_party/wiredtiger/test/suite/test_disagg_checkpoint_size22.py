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

from collections import namedtuple
from contextlib import closing
from pathlib import Path
from helper_disagg import DisaggSizeTestMixin, disagg_test_class
from wiredtiger import stat
import wttest


@disagg_test_class
class test_disagg_checkpoint_size22(DisaggSizeTestMixin, wttest.WiredTigerTestCase):
    """These tests assess how size-only statistics work for different tables on disagg connections"""

    conn_config = 'disaggregated=(role="leader",lose_all_my_data=true)'
    base_config = "key_format=S,value_format=S"

    Table = namedtuple("Table", ["uri", "file_name", "config"])

    layered_table = Table(
        f"table:{__qualname__}",
        f"{__qualname__}.wt_stable",
        base_config + ",block_manager=disagg,type=layered",
    )

    local_table = Table(
        f"table:{__qualname__}_local",
        f"{__qualname__}_local.wt",
        base_config + ",block_manager=default,type=file",
    )

    def open_and_populate(self, table):
        """Open the table and insert enough rows to give it a nontrivial size."""
        self.session.create(table.uri, table.config)
        with wttest.open_cursor(self.session, table.uri) as cursor:
            for i in range(1000):
                cursor[f"key{i:08d}"] = "x" * 100

    def open_layered_table(self):
        """Populate and checkpoint the layered table, returning its stable checkpoint size."""
        self.open_and_populate(self.layered_table)
        self.session.checkpoint()
        size = self.get_checkpoint_size("file:" + self.layered_table.file_name)
        self.assertGreater(size, 0)
        return size

    def size_stat(self, key, uri=None, session=None):
        """Return one statistic from a size-only statistics cursor."""
        with wttest.open_cursor(
            session or self.session,
            f"statistics:{uri or self.layered_table.uri}",
            config="statistics=(size)",
        ) as cursor:
            return cursor[key][2]

    def block_size(self, uri=None, session=None):
        """Return the block size reported for a table."""
        return self.size_stat(stat.dsrc.block_size, uri, session)

    def block_manager_consulted(self, uri=None, session=None):
        """Return whether size collection called the block manager statistics method."""
        # The fast path zeroes the statistics and fills only block_size. block_magic is
        # written only by a block manager's stat call.
        return self.size_stat(stat.dsrc.block_magic, uri, session) != 0

    def dhandle_count(self, session=None):
        """Return the number of active dhandles on the connection."""
        with wttest.open_cursor(session or self.session, "statistics:") as cursor:
            return cursor[stat.conn.dh_conn_handle_count][2]

    def test_layered_table_size_uses_checkpoint_meta(self):
        """A leader reports a layered table's checkpoint size without opening dhandles."""
        expected_size = self.open_layered_table()

        # Reopen to drop the cached dhandles.
        with self.expectedStdoutPattern("Removing local file"):
            self.reopen_conn()

        dhandles_before = self.dhandle_count()
        self.assertEqual(self.block_size(), expected_size)
        self.assertEqual(self.dhandle_count(), dhandles_before)

    def test_layered_table_follower_uses_checkpoint_meta(self):
        """A follower reports the picked-up checkpoint size without opening dhandles."""
        expected_size = self.open_layered_table()

        # A follower that has picked up the leader's checkpoint but never opened the table.
        follower_config = (
            self.extensionsConfig()
            + ",create,statistics=(fast),"
            + f'disaggregated=(page_log={self.page_log()},role="follower")'
        )

        with closing(
            self.wiredtiger_open("follower", follower_config)
        ) as follower_connection:
            self.disagg_advance_checkpoint(follower_connection)
            follower_session = follower_connection.open_session("")

            dhandles_before = self.dhandle_count(follower_session)
            self.assertEqual(self.block_size(session=follower_session), expected_size)
            self.assertFalse(self.block_manager_consulted(session=follower_session))
            self.assertEqual(self.dhandle_count(follower_session), dhandles_before)

            follower_session.close()

    def test_local_table_size_on_disagg_uses_filesystem(self):
        """A local table on a disagg connection reports its filesystem size."""
        self.open_and_populate(self.local_table)
        self.session.checkpoint()

        self.assertEqual(
            self.block_size(self.local_table.uri),
            Path(self.local_table.file_name).stat().st_size,
        )
        self.assertFalse(self.block_manager_consulted(self.local_table.uri))

    def test_layered_table_size_without_checkpoint_uses_slow_path(self):
        """A layered table with no checkpoint falls back to the slow path."""
        self.open_and_populate(self.layered_table)

        # No checkpoint has occurred, so we expect the slow path to be taken.
        self.assertTrue(self.block_manager_consulted())


if __name__ == "__main__":
    wttest.run()
