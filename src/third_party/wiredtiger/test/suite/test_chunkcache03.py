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

import os, sys
import wiredtiger, wttest

from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

'''
- Evaluate chunkcache performance both in-memory and on-disk, to test the functionality of pinned chunks.
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

    scenarios = make_scenarios(format_values, cache_types)

    def conn_config(self):
        if not os.path.exists('bucket2'):
            os.mkdir('bucket2')

        return 'tiered_storage=(auth_token=Secret,bucket=bucket2,bucket_prefix=pfx_,name=dir_store),' \
            'chunk_cache=[enabled=true,chunk_size=512KB,capacity=20GB,pinned=("table:chunkcache01", "table:chunkcache02"),type={},storage_path=WiredTigerChunkCache]'.format(self.chunk_cache_type)

    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('storage_sources', 'dir_store')

    def get_stat(self, stat):
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
        uris = ["table:chunkcache01", "table:chunkcache02", "table:chunkcache03", "table:chunkcache04"]
        ds = [SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format) for uri in uris]

        # Insert data in four tables.
        for i, dataset in enumerate(ds):
            dataset.populate()
            self.insert(uris[i], dataset)

        self.session.checkpoint()
        self.session.checkpoint('flush_tier=(enabled)')

        # Reopen wiredtiger to migrate all data to disk.
        self.reopen_conn()

        # Read from the unpinned URIs and capture chunks in use.
        self.read_and_verify(uris[2], ds[2])
        self.read_and_verify(uris[3], ds[3])
        chunks_inuse_excluding_pinned = self.get_stat(wiredtiger.stat.conn.chunk_cache_chunks_inuse)
        self.assertGreater(chunks_inuse_excluding_pinned, 0)

        # Assert none of the chunks are pinned.
        self.assertEqual(self.get_stat(wiredtiger.stat.conn.chunk_cache_chunks_pinned), 0)

        # Read from the pinned URIs.
        self.read_and_verify(uris[0], ds[0])
        self.read_and_verify(uris[1], ds[1])
        chunks_inuse_including_pinned = self.get_stat(wiredtiger.stat.conn.chunk_cache_chunks_inuse)
        self.assertGreater(chunks_inuse_including_pinned, chunks_inuse_excluding_pinned)

        # Assert that chunks are pinned.
        pinned_chunks_inuse = self.get_stat(wiredtiger.stat.conn.chunk_cache_chunks_pinned)
        self.assertGreater(pinned_chunks_inuse, 0)

        # Assert that the difference b/w the total chunks present and the unpinned chunks equal pinned chunks.
        # This proves that the chunks read from pinned objects were all pinned.
        self.assertEqual(chunks_inuse_including_pinned - chunks_inuse_excluding_pinned, pinned_chunks_inuse)

        # Reconfigure wiredtiger and mark the pinned objects as unpinned and vice-versa.
        self.conn.reconfigure('chunk_cache=[pinned=("table:chunkcache03", "table:chunkcache04")]')

        # After this point all the unpinned chunks should be pinned and vice-versa.
        # Check the following stats.
        new_pinned_chunks_inuse = self.get_stat(wiredtiger.stat.conn.chunk_cache_chunks_pinned)
        self.assertEqual(chunks_inuse_excluding_pinned, new_pinned_chunks_inuse)
