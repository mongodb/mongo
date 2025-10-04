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

import os, sys, time, wiredtiger, wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

def get_stat(session, stat):
    stat_cursor = session.open_cursor('statistics:')
    val = stat_cursor[stat][2]
    stat_cursor.close()

    return val

# Read a stat either 10,000 times (or for 10 seconds), whichever is greater,
# unless the condition is met. Raises an exception since it doesn't have
# access to assertGreater and friends.
def stat_cond_timeout(session, stat, cond):
    start = time.time()
    val = get_stat(session, stat)
    elapsed = 0
    iterations = 0
    while (not cond(val)) and (elapsed < 10 or iterations < 10000):
        val = get_stat(session, stat)
        elapsed = time.time() - start
        iterations += 1

    if not cond(val):
        raise Exception("stat {} ({}) failed check".format(stat, val))

def stat_assert_equal(session, stat, expected):
    stat_cond_timeout(session, stat, lambda x: x == expected)

def stat_assert_greater(session, stat, expected):
    stat_cond_timeout(session, stat, lambda x: x > expected)

# Basic functional chunk cache test - put some data in, make sure it
# comes back out unscathed.
class test_chunkcache01(wttest.WiredTigerTestCase):
    uri = 'table:test_chunkcache01'

    format_values = [
        ('column-fix', dict(key_format='r', value_format='8t')),
        ('column', dict(key_format='r', value_format='u')),
        ('row_string', dict(key_format='S', value_format='u')),
    ]

    cache_types = [('in-memory', dict(chunk_cache_type='DRAM'))]
    if sys.byteorder == 'little':
        # WT's filesystem layer doesn't support mmap on big-endian platforms.
        cache_types.append(('on-disk', dict(chunk_cache_type='FILE')))

    scenarios = make_scenarios(format_values, cache_types)

    def conn_config(self):
        if not os.path.exists('bucket1'):
            os.mkdir('bucket1')

        return 'tiered_storage=(auth_token=Secret,bucket=bucket1,bucket_prefix=pfx_,name=dir_store),' \
            'chunk_cache=[enabled=true,chunk_size=1MB,capacity=20MB,type={},storage_path=WiredTigerChunkCache],'.format(self.chunk_cache_type)

    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('storage_sources', 'dir_store')

    def test_chunkcache(self):
        ds = SimpleDataSet(self, self.uri, 10, key_format=self.key_format, value_format=self.value_format)
        ds.populate()
        ds.check()
        self.session.checkpoint()
        self.session.checkpoint('flush_tier=(enabled)')
        ds.check()

        # Assert the new chunks are ingested.
        stat_assert_greater(self.session, wiredtiger.stat.conn.chunkcache_chunks_loaded_from_flushed_tables, 0)

        self.close_conn()
        self.reopen_conn()
        ds.check()
        stat_assert_greater(self.session, wiredtiger.stat.conn.chunkcache_bytes_inuse, 0)
