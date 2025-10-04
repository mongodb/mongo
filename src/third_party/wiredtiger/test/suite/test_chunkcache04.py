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

from test_chunkcache01 import stat_assert_equal, stat_assert_greater
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

'''
Functional testing for ingesting new content into the chunk cache.
- Verify ingests are taking place with both pinned and unpinned chunks.
- Verify that when ingesting new chunks with old pinned objects, we are releasing the pin on the old objects.
'''
class test_chunkcache04(wttest.WiredTigerTestCase):
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

    def test_chunkcache04(self):
        uris = ["table:chunkcache03", "table:chunkcache04"]
        ds = [SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format) for uri in uris]

        # Insert unpinned data into two tables.
        for i, dataset in enumerate(ds):
            dataset.populate()
            self.insert(uris[i], dataset)

        # As we have not flushed yet, assert we have no newly inserted chunks.
        stat_assert_equal(self.session, wiredtiger.stat.conn.chunkcache_chunks_loaded_from_flushed_tables, 0)

        # Flush the unpinned tables into the chunk cache.
        self.session.checkpoint()
        self.session.checkpoint('flush_tier=(enabled)')

        # Assert that chunks are not pinned.
        stat_assert_equal(self.session, wiredtiger.stat.conn.chunkcache_chunks_pinned, 0)

        # Assert the new chunks are ingested.
        first_ingest = self.get_stat(wiredtiger.stat.conn.chunkcache_chunks_loaded_from_flushed_tables)
        self.assertGreater(first_ingest, 0)

        ds2 = [SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format) for uri in self.pinned_uris]

        # Insert pinned data into two tables.
        for i, dataset in enumerate(ds2):
            dataset.populate()
            self.insert(self.pinned_uris[i], dataset)

        # Flush the pinned tables into the chunk cache.
        self.session.checkpoint()
        self.session.checkpoint('flush_tier=(enabled)')

        # Assert the new chunks are ingested.
        second_ingest = self.get_stat(wiredtiger.stat.conn.chunkcache_chunks_loaded_from_flushed_tables)
        self.assertGreater(second_ingest, first_ingest)

        # Assert that chunks are pinned.
        old_pinned = self.get_stat(wiredtiger.stat.conn.chunkcache_chunks_pinned)
        self.assertGreater(old_pinned, 0)

        # Modify the tables content so flush has work to do.
        cursor = self.session.open_cursor(self.pinned_uris[0])
        cursor[ds2[0].key(1)] = 'foo'
        cursor1 = self.session.open_cursor(self.pinned_uris[1])
        cursor1[ds2[1].key(2)] = 'bar'

        # Flush the pinned tables into the chunk cache.
        self.session.checkpoint()
        self.session.checkpoint('flush_tier=(enabled)')

        # Assert another set of ingests took place.
        total_ingest = self.get_stat(wiredtiger.stat.conn.chunkcache_chunks_loaded_from_flushed_tables)
        self.assertGreater(total_ingest, second_ingest)

        # Assert the old chunks from the pinned table were unset after flush.
        self.assertGreater(old_pinned, self.get_stat(wiredtiger.stat.conn.chunkcache_chunks_pinned))
