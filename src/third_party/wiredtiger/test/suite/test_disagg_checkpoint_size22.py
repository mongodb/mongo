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

import errno
import os
import re
from collections import namedtuple
from contextlib import closing
from pathlib import Path
from helper_disagg import DisaggSizeTestMixin, disagg_test_class
import wiredtiger
from wiredtiger import stat
import wttest


@disagg_test_class
class test_disagg_checkpoint_size22(DisaggSizeTestMixin, wttest.WiredTigerTestCase):
    """Test size-only statistics for local and shared tables and files."""

    conn_config = 'disaggregated=(role="leader",lose_all_my_data=true)'
    base_config = "key_format=S,value_format=S"

    Table = namedtuple("Table", ["uri", "file_name", "config"])

    layered_table = Table(
        f"table:{__qualname__}",
        f"{__qualname__}.wt_stable",
        base_config + ",type=layered",
    )

    local_table = Table(
        f"table:{__qualname__}_local",
        f"{__qualname__}_local.wt",
        base_config + ",block_manager=default,type=file",
    )

    shared_table = Table(
        f"table:{__qualname__}_shared",
        f"{__qualname__}_shared.wt",
        base_config + ",block_manager=disagg,type=file",
    )

    def create_and_populate(self, table):
        """Create the table and insert enough rows to give it a nontrivial size."""
        self.session.create(table.uri, table.config)
        with wttest.open_cursor(self.session, table.uri) as cursor:
            for i in range(1000):
                cursor[f"key{i:08d}"] = "x" * 100

    def create_and_checkpoint_layered_table(self):
        """Create and checkpoint the layered table, returning its nonzero stable size."""
        self.create_and_populate(self.layered_table)
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
        """Return the reported block size."""
        return self.size_stat(stat.dsrc.block_size, uri, session)

    def used_dhandle_stats_path(self, uri=None, session=None):
        """Return whether size collection used the dhandle block manager statistics path."""
        # The fast path fills only block_size. A dhandle block manager stat call also fills
        # block_magic.
        return self.size_stat(stat.dsrc.block_magic, uri, session) != 0

    def dhandle_count(self, session=None):
        """Return the number of active dhandles on the connection."""
        with wttest.open_cursor(session or self.session, "statistics:") as cursor:
            return cursor[stat.conn.dh_conn_handle_count][2]

    def follower_config(self):
        """Return the configuration for a follower connection with statistics enabled."""
        return (
            self.extensionsConfig()
            + ",create,statistics=(fast),"
            + f'disaggregated=(page_log={self.page_log()},role="follower")'
        )

    def test_layered_table_uses_file_metadata_fast_path(self):
        """A layered table uses the file-metadata fast path without allocating a dhandle."""
        expected_size = self.create_and_checkpoint_layered_table()

        # Reopen to drop the cached dhandles.
        with self.expectedStdoutPattern("Removing local file"):
            self.reopen_conn()

        dhandles_before = self.dhandle_count()
        self.assertEqual(self.block_size(), expected_size)
        self.assertFalse(self.used_dhandle_stats_path())
        self.assertEqual(self.dhandle_count(), dhandles_before)

    def test_layered_table_on_follower_uses_file_metadata_fast_path(self):
        """A follower uses the file-metadata fast path without allocating a dhandle."""
        expected_size = self.create_and_checkpoint_layered_table()

        # A follower that has picked up the leader's checkpoint but never opened the table.
        with closing(
            self.wiredtiger_open("follower", self.follower_config())
        ) as follower_connection:
            self.disagg_advance_checkpoint(follower_connection)
            with closing(follower_connection.open_session("")) as follower_session:
                dhandles_before = self.dhandle_count(follower_session)
                self.assertEqual(
                    self.block_size(session=follower_session), expected_size
                )
                self.assertFalse(self.used_dhandle_stats_path(session=follower_session))
                self.assertEqual(self.dhandle_count(follower_session), dhandles_before)

    def test_layered_table_zero_size_on_follower_uses_file_metadata_fast_path(self):
        """A follower reports zero through the file-metadata fast path."""
        self.session.create(self.layered_table.uri, self.layered_table.config)
        self.session.checkpoint()

        stable_uri = "file:" + self.layered_table.file_name
        self.assertEqual(self.get_checkpoint_size(stable_uri), 0)

        with closing(
            self.wiredtiger_open("follower", self.follower_config())
        ) as follower_connection:
            self.disagg_advance_checkpoint(follower_connection)
            with closing(follower_connection.open_session("")) as follower_session:
                dhandles_before = self.dhandle_count(follower_session)
                self.assertEqual(self.block_size(session=follower_session), 0)
                self.assertFalse(self.used_dhandle_stats_path(session=follower_session))
                self.assertEqual(self.dhandle_count(follower_session), dhandles_before)

    def test_local_table_uses_filesystem_fast_path(self):
        """A local table uses the filesystem fast path."""
        self.create_and_populate(self.local_table)
        self.session.checkpoint()

        self.assertEqual(
            self.block_size(self.local_table.uri),
            Path(self.local_table.file_name).stat().st_size,
        )
        self.assertFalse(self.used_dhandle_stats_path(self.local_table.uri))

    def test_local_file_uses_filesystem_fast_path(self):
        """A local file uses the filesystem fast path."""
        self.session.create(self.local_table.uri, self.local_table.config)
        file_uri = "file:" + self.local_table.file_name

        self.assertEqual(
            self.block_size(file_uri), Path(self.local_table.file_name).stat().st_size
        )
        self.assertFalse(self.used_dhandle_stats_path(file_uri))

    def test_shared_table_uses_file_metadata_fast_path(self):
        """A shared table uses the file-metadata fast path."""
        self.create_and_populate(self.shared_table)
        file_uri = "file:" + self.shared_table.file_name

        self.assertEqual(self.block_size(self.shared_table.uri), 0)
        self.assertFalse(self.used_dhandle_stats_path(self.shared_table.uri))

        self.session.checkpoint()
        expected_size = self.get_checkpoint_size(file_uri)
        self.assertGreater(expected_size, 0)
        self.assertEqual(self.block_size(self.shared_table.uri), expected_size)
        self.assertFalse(self.used_dhandle_stats_path(self.shared_table.uri))

    def test_shared_file_uses_file_metadata_fast_path(self):
        """A shared file uses the file-metadata fast path."""
        self.create_and_populate(self.shared_table)
        file_uri = "file:" + self.shared_table.file_name

        self.assertEqual(self.block_size(file_uri), 0)
        self.assertFalse(self.used_dhandle_stats_path(file_uri))

        self.session.checkpoint()
        expected_size = self.get_checkpoint_size(file_uri)
        self.assertGreater(expected_size, 0)
        self.assertEqual(self.block_size(file_uri), expected_size)
        self.assertFalse(self.used_dhandle_stats_path(file_uri))

    def test_indexed_table_uses_schema_path(self):
        """An indexed table uses schema aggregation instead of the simple-table fast path."""
        name = f"{self.__class__.__name__}_indexed"
        table_uri = f"table:{name}"
        file_uri = f"file:{name}.wt"
        index_uri = f"index:{name}:value"

        self.session.create(table_uri, self.base_config + ",columns=(key,value)")
        self.session.create(index_uri, "columns=(value)")
        with wttest.open_cursor(self.session, table_uri) as cursor:
            for i in range(1000):
                cursor[f"key{i:08d}"] = f"value{i:08d}"
        self.session.checkpoint()

        file_size = self.block_size(file_uri)
        index_size = self.block_size(index_uri)
        self.assertGreater(index_size, 0)
        self.assertEqual(self.block_size(table_uri), file_size + index_size)

    def test_layered_table_before_first_checkpoint_uses_file_metadata_fast_path(self):
        """A layered table uses the file-metadata fast path before its first checkpoint."""
        self.create_and_populate(self.layered_table)

        self.assertEqual(self.block_size(), 0)
        self.assertFalse(self.used_dhandle_stats_path())

    def test_layered_table_fake_checkpoint_uses_file_metadata_fast_path(self):
        """A layered table uses the file-metadata fast path for a fake checkpoint."""
        self.session.create(self.layered_table.uri, self.layered_table.config)
        self.session.checkpoint()

        stable_uri = "file:" + self.layered_table.file_name
        expected_size = self.get_checkpoint_size(stable_uri)

        # Ensure that it was a fake checkpoint.
        with wttest.open_cursor(self.session, "metadata:") as cursor:
            metadata = cursor[stable_uri]
        self.assertEqual(metadata.count("WiredTigerCheckpoint."), 1)
        self.assertIn('addr=""', metadata)

        self.assertEqual(expected_size, 0)
        self.assertEqual(self.block_size(), expected_size)
        self.assertFalse(self.used_dhandle_stats_path())

    def test_layered_table_post_delete_checkpoint_uses_file_metadata_fast_path(self):
        """A layered table uses the file-metadata fast path after deleting its last key."""
        self.session.create(self.layered_table.uri, self.layered_table.config)
        with wttest.open_cursor(self.session, self.layered_table.uri) as cursor:
            cursor["key"] = "value"
        self.session.checkpoint()

        stable_uri = "file:" + self.layered_table.file_name
        self.assertGreater(self.get_checkpoint_size(stable_uri), 0)

        with wttest.open_cursor(self.session, self.layered_table.uri) as cursor:
            cursor.set_key("key")
            self.assertEqual(cursor.remove(), 0)
        self.session.checkpoint()

        self.assertEqual(self.get_checkpoint_size(stable_uri), 0)
        self.assertEqual(self.block_size(), 0)
        self.assertFalse(self.used_dhandle_stats_path())

    def test_stable_file_before_first_checkpoint_uses_file_metadata_fast_path(self):
        """A stable file uses the file-metadata fast path before its first checkpoint."""
        self.session.create(self.layered_table.uri, self.layered_table.config)

        stable_uri = "file:" + self.layered_table.file_name
        self.assertEqual(self.block_size(stable_uri), 0)
        self.assertFalse(self.used_dhandle_stats_path(stable_uri))

    def test_stable_file_uses_file_metadata_fast_path(self):
        """A stable file uses the file-metadata fast path without allocating a dhandle."""
        expected_size = self.create_and_checkpoint_layered_table()
        stable_uri = "file:" + self.layered_table.file_name

        # Reopen to drop the cached dhandles.
        with self.expectedStdoutPattern("Removing local file"):
            self.reopen_conn()

        dhandles_before = self.dhandle_count()
        self.assertEqual(self.block_size(stable_uri), expected_size)
        self.assertFalse(self.used_dhandle_stats_path(stable_uri))
        self.assertEqual(self.dhandle_count(), dhandles_before)

    def test_stable_checkpoint_view_uses_dhandle_stats_path(self):
        """A stable checkpoint-view URI uses the dhandle statistics path."""
        expected_size = self.create_and_checkpoint_layered_table()
        stable_uri = "file:" + self.layered_table.file_name

        with wttest.open_cursor(self.session, "metadata:") as cursor:
            metadata = cursor[stable_uri]
        checkpoint_names = re.findall(r"(WiredTigerCheckpoint\.\d+)=", metadata)
        self.assertTrue(checkpoint_names)

        checkpoint_view_uri = f"{stable_uri}/{checkpoint_names[-1]}"
        self.assertEqual(self.block_size(checkpoint_view_uri), expected_size)
        self.assertTrue(self.used_dhandle_stats_path(checkpoint_view_uri))

    def test_missing_file_without_metadata_returns_enoent_without_dhandle(self):
        """A missing file without metadata returns ENOENT without allocating a dhandle."""
        uri = f"file:{self.__class__.__name__}_missing.wt"
        dhandles_before = self.dhandle_count()
        self.assertRaisesException(
            wiredtiger.WiredTigerError,
            lambda: self.block_size(uri),
            os.strerror(errno.ENOENT),
        )
        self.assertEqual(self.dhandle_count(), dhandles_before)

    def test_existing_file_without_metadata_returns_enoent_without_dhandle(self):
        """An existing filesystem file without metadata returns ENOENT without allocating a dhandle."""
        filename = f"{self.__class__.__name__}_without_metadata.wt"
        Path(filename).write_bytes(b"not a WiredTiger file")

        uri = "file:" + filename
        dhandles_before = self.dhandle_count()
        self.assertRaisesException(
            wiredtiger.WiredTigerError,
            lambda: self.block_size(uri),
            os.strerror(errno.ENOENT),
        )
        self.assertEqual(self.dhandle_count(), dhandles_before)

    def test_local_file_with_metadata_missing_from_disk_uses_dhandle_path(self):
        """A local file with metadata but no filesystem file falls back to the dhandle path."""
        self.create_and_populate(self.local_table)
        self.session.checkpoint()

        self.close_conn()
        Path(self.local_table.file_name).unlink()
        self.open_conn(config='disaggregated=(role="leader")')

        file_uri = "file:" + self.local_table.file_name
        self.assertFalse(Path(self.local_table.file_name).exists())
        with wttest.open_cursor(self.session, "metadata:") as cursor:
            cursor.set_key(file_uri)
            self.assertEqual(cursor.search(), 0)

        dhandles_before = self.dhandle_count()
        self.assertRaisesException(
            wiredtiger.WiredTigerError,
            lambda: self.block_size(file_uri),
            os.strerror(errno.ENOENT),
        )
        self.assertGreater(self.dhandle_count(), dhandles_before)

    def test_missing_table_without_metadata_returns_enoent_without_dhandle(self):
        """A missing table without metadata returns ENOENT without allocating a dhandle."""
        uri = f"table:{self.__class__.__name__}_missing"
        dhandles_before = self.dhandle_count()
        self.assertRaisesException(
            wiredtiger.WiredTigerError,
            lambda: self.block_size(uri),
            os.strerror(errno.ENOENT),
        )
        self.assertEqual(self.dhandle_count(), dhandles_before)


if __name__ == "__main__":
    wttest.run()
