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

import os, random, sys, time
import wiredtiger, wttest

from test_chunkcache01 import stat_assert_equal, stat_assert_greater
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

'''
Functional testing for chunk cache persistence. Verifies that corruption on disk is caught and
dealt with.
'''
class test_chunkcache06(wttest.WiredTigerTestCase):
    rows = 10000
    uri = "table:chunkcache06"

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('row_string', dict(key_format='S', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def conn_config(self):
        if sys.byteorder != 'little':
            return ''

        if not os.path.exists('bucket6'):
            os.mkdir('bucket6')

        return 'tiered_storage=(auth_token=Secret,bucket=bucket6,bucket_prefix=pfx_,name=dir_store),' \
            'chunk_cache=[enabled=true,chunk_size=512KB,capacity=10MB,type=FILE,' \
            'storage_path=WiredTigerChunkCache]'

    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('storage_sources', 'dir_store')

    # Corrupt the chunk cache content in 1-10 places, with a run of 1-100 bytes.
    def corrupt_random_chunk_cache_data(self):
        corruptions = random.randrange(1, 11)
        size = os.path.getsize('WiredTigerChunkCache')

        with open('WiredTigerChunkCache', 'w') as f:
            for _ in range(corruptions):
                run = random.randrange(1, 101)
                pos = random.randrange(0, size - run)
                bad_bytes = str([random.randrange(0, 256) for _ in range(run)])
                f.seek(pos)
                f.write(bad_bytes)

    def test_chunkcache06(self):
        # This test only makes sense on-disk, and WT's filesystem layer doesn't support mmap on
        # big-endian platforms.
        if sys.byteorder != 'little':
            return

        ds = SimpleDataSet(self, self.uri, self.rows, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        # Haven't persisted anything yet - check the stats agree.
        stat_assert_equal(self.session, wiredtiger.stat.conn.chunkcache_created_from_metadata, 0)
        stat_assert_equal(self.session, wiredtiger.stat.conn.chunkcache_bytes_read_persistent, 0)

        # Flush the tables into the chunk cache.
        self.session.checkpoint()
        self.session.checkpoint('flush_tier=(enabled)')

        # Make sure we write out the metadata entries we're planning to read back.
        stat_assert_greater(self.session, wiredtiger.stat.conn.chunkcache_metadata_inserted, 0)

        self.close_conn()

        # Damage the underlying file while WT isn't running.
        self.corrupt_random_chunk_cache_data()

        self.reopen_conn()

        # Assert the chunks are read back in on startup. Wait for the stats to indicate
        # that it's done the work.
        stat_assert_greater(self.session, wiredtiger.stat.conn.chunkcache_created_from_metadata, 0)
        stat_assert_greater(self.session, wiredtiger.stat.conn.chunkcache_bytes_read_persistent, 0)

        # Check that our data is all intact, despite having to reload chunks.
        ds.check()
        stat_assert_greater(self.session, wiredtiger.stat.conn.chunkcache_retries_checksum_mismatch, 0)
