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

from test_chunkcache01 import stat_assert_equal, stat_assert_greater
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

'''
Functional testing for chunk cache persistence. Verifies that persistent
content is loaded back in using stats.
'''
class test_chunkcache05(wttest.WiredTigerTestCase):
    rows = 10000
    uri = "table:chunkcache05"

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('row_string', dict(key_format='S', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def conn_config(self):
        if sys.byteorder != 'little':
            return ''

        if not os.path.exists('bucket5'):
            os.mkdir('bucket5')

        return 'tiered_storage=(auth_token=Secret,bucket=bucket5,bucket_prefix=pfx_,name=dir_store),' \
            'chunk_cache=[enabled=true,chunk_size=512KB,capacity=10MB,type=FILE,' \
            'storage_path=WiredTigerChunkCache]'

    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('storage_sources', 'dir_store')

    def test_chunkcache05(self):
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
        self.reopen_conn()

        # Assert the chunks are read back in on startup. Wait for the stats to indicate
        # that it's done the work.
        stat_assert_greater(self.session, wiredtiger.stat.conn.chunkcache_created_from_metadata, 0)
        stat_assert_greater(self.session, wiredtiger.stat.conn.chunkcache_bytes_read_persistent, 0)

        # Check that our data is all intact.
        ds.check()
