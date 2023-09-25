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
import random
import threading
import time
import wiredtiger, wttest

from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

'''
Testing chunkcache in-memory and on-disk.

Create a multithreaded environment to allow for the allocation and deallocation of bits
in the bitmap (for the on-disk case) and chunks.
'''
class test_chunkcache02(wttest.WiredTigerTestCase):
    uri = "table:test_chunkcache02"
    rows = 10000
    num_threads = 5

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
            'chunk_cache=[enabled=true,chunk_size=512KB,capacity=20MB,type={},storage_path=WiredTigerChunkCache],'.format(self.chunk_cache_type)

    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('storage_sources', 'dir_store')

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def read_and_verify(self, rows, ds):
        session = self.conn.open_session()
        cursor = session.open_cursor(self.uri)
        for i in range(1, rows * 10):
            key = random.randint(1, rows - 1)
            cursor.set_key(ds.key(key))
            cursor.search()
            self.assertEqual(cursor.get_value(), str(key) * self.rows)

    def test_chunkcache02(self):
        ds = SimpleDataSet(
            self, self.uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        # Insert a large amount of data.
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.rows):
            cursor[ds.key(i)] = str(i) * self.rows

        self.session.checkpoint()
        self.session.checkpoint('flush_tier=(enabled)')

        # Reopen wiredtiger to migrate all data to disk.
        self.reopen_conn()

        '''
        Allow a specified number of threads to perform reads and data verification.
        This action forces the chunks to be cached, considering the intentionally
        small size of the chunk cache. This process also triggers eviction,
        testing both chunk allocation and deallocation. It's important to note
        that there's a possibility of re-reading freed chunks which only extends the scope
        of this testing.
        '''
        threads = []
        for _ in range(self.num_threads):
            thread = threading.Thread(target=self.read_and_verify, args=(self.rows, ds,))
            threads.append(thread)
            thread.start()

        for thread in threads:
            thread.join()

        # Check relevant chunkcache stats.
        self.assertGreater(self.get_stat(wiredtiger.stat.conn.chunk_cache_chunks_inuse), 0)
        self.assertGreater(self.get_stat(wiredtiger.stat.conn.chunk_cache_chunks_evicted), 0)
