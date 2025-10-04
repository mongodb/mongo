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

import os, sys, time
import wiredtiger, wttest

from test_chunkcache01 import stat_assert_greater
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

'''
- Evaluate chunk cache performance both in-memory and on-disk, to test the functionality of pinned chunks.
- Verify the functionality of reconfiguring pinned configurations.
'''
class test_chunkcache03(wttest.WiredTigerTestCase):
    rows = 10000

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('row_string', dict(key_format='S', value_format='S')),
    ]

    cache_types = [('in-memory', dict(chunk_cache_type='DRAM'))]
    if sys.byteorder == 'little':
        # WT's filesystem layer doesn't support mmap on big-endian platforms.
        cache_types.append(('on-disk', dict(chunk_cache_type='FILE')))

    pinned_uris = ["table:chunkcache01", "table:chunkcache02"]

    scenarios = make_scenarios(format_values, cache_types)

    def conn_config(self):
        if not os.path.exists('bucket2'):
            os.mkdir('bucket2')

        return 'tiered_storage=(auth_token=Secret,bucket=bucket2,bucket_prefix=pfx_,name=dir_store),' \
            'chunk_cache=[enabled=true,chunk_size=512KB,capacity=1GB,pinned=' \
                + '("' + '{}'.format("\",\"".join(self.pinned_uris)) \
                + '"),type={},storage_path=WiredTigerChunkCache]'.format(self.chunk_cache_type)

    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('storage_sources', 'dir_store')

    def get_stat(self, stat):
        time.sleep(0.5) # Try to avoid race conditions.
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def insert(self, uri, ds):
        cursor = self.session.open_cursor(uri)
        for i in range(1, self.rows):
            cursor[ds.key(i)] = str(i) * 100

    def read_and_verify(self, uri, ds):
        cursor = self.session.open_cursor(uri)
        for i in range(1, self.rows):
            cursor.set_key(ds.key(i))
            cursor.search()
            self.assertEqual(cursor.get_value(), str(i) * 100)

    def test_chunkcache03(self):
        uris = self.pinned_uris + ["table:chunkcache03", "table:chunkcache04"]
        ds = [SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format) for uri in uris]

        # Insert data into four tables.
        for i, dataset in enumerate(ds):
            dataset.populate()
            self.insert(uris[i], dataset)

        self.session.checkpoint()
        self.session.checkpoint('flush_tier=(enabled)')

        # Assert the new chunks are ingested.
        stat_assert_greater(self.session, wiredtiger.stat.conn.chunkcache_chunks_loaded_from_flushed_tables, 0)

        # Reopen wiredtiger to migrate all data to disk.
        self.reopen_conn()

        # Ensure chunks are read from metadata in type=FILE case.
        if self.chunk_cache_type == "FILE":
            stat_assert_greater(self.session, wiredtiger.stat.conn.chunkcache_created_from_metadata, 0)

        # For the type=DRAM case read manually to cache the chunks.
        for i in range(0, 4):
            self.read_and_verify(uris[i], ds[i])

        # Assert pinned/unpinned stats.
        total_chunks = self.get_stat(wiredtiger.stat.conn.chunkcache_chunks_inuse)
        pinned_chunks = self.get_stat(wiredtiger.stat.conn.chunkcache_chunks_pinned)
        self.assertGreater(total_chunks, 0)
        self.assertGreater(pinned_chunks, 0)

        # Assert that pinning a table also pins its backing chunks.
        # We can't measure this directly, so exploit the fact that all four tables have been
        # populated to roughly the same size and will use ~25% of available chunks each.

        # We start with two tables pinned. This is half the total chunks
        self.assertEqual(round(total_chunks/pinned_chunks), 2)

        # Now reconfigure to unpin the second table for a quarter of all chunks.
        self.conn.reconfigure('chunk_cache=[pinned=("table:chunkcache01")]')
        total_chunks = self.get_stat(wiredtiger.stat.conn.chunkcache_chunks_inuse)
        pinned_chunks = self.get_stat(wiredtiger.stat.conn.chunkcache_chunks_pinned)
        self.assertEqual(round(total_chunks/pinned_chunks), 4)

        # Now pin two different tables again for half the chunks.
        self.conn.reconfigure('chunk_cache=[pinned=("table:chunkcache02", "table:chunkcache04")]')
        total_chunks = self.get_stat(wiredtiger.stat.conn.chunkcache_chunks_inuse)
        pinned_chunks = self.get_stat(wiredtiger.stat.conn.chunkcache_chunks_pinned)
        self.assertEqual(round(total_chunks/pinned_chunks), 2)

        # Finally pin all tables to pin all chunks.
        self.conn.reconfigure('chunk_cache=[pinned=("table:chunkcache01", "table:chunkcache02", "table:chunkcache03", "table:chunkcache04")]')
        total_chunks = self.get_stat(wiredtiger.stat.conn.chunkcache_chunks_inuse)
        pinned_chunks = self.get_stat(wiredtiger.stat.conn.chunkcache_chunks_pinned)
        self.assertEqual(round(total_chunks/pinned_chunks), 1)
