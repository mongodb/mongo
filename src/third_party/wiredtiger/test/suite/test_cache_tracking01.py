#!/usr/bin/env python
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
from wtscenario import make_scenarios
from wtdataset import SimpleDataSet

# Ensure that cache size is tracked correctly when the content on a disk page is smaller than the page itself.
class test_cache_tracking(wttest.WiredTigerTestCase):
    uri = "table:test_cache_tracking"
    conn_config = "cache_size=1GB,statistics=(all),statistics_log=(wait=1,json=true,on_close=true)"

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]
    compressors = [
        ('none', dict(compressor='none')),
        ('snappy', dict(compressor='snappy')),
    ]
    scenarios = make_scenarios(format_values, compressors)

    # Load the compressor extension, skip the test if missing.
    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        extlist.extension('compressors', self.compressor)

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def test_cache_tracking(self):
        _128MB = 128000000
        nrows = 10
        conf = 'allocation_size=128MB,leaf_page_max=128MB,internal_page_max=128MB,' \
            'block_compressor={}'.format(self.compressor)
        
        ds = SimpleDataSet(
            self, self.uri, nrows, key_format=self.key_format, value_format=self.value_format, config=conf)
        ds.create()
        cursor = ds.open_cursor(self.uri, None)

        for i in range(1, nrows):
            cursor[ds.key(i)] = ds.value(i)

        # Evict pages to disk.
        evict_cursor = self.session.open_cursor(self.uri, None, "debug=(release_evict)")
        for i in range(1, nrows):
            evict_cursor.set_key(ds.key(i))
            self.assertEquals(evict_cursor.search(), 0)
            evict_cursor.reset()

        # Read a page back in memory.
        cursor.set_key(ds.key(1))
        cursor.search()

        # This test aims to validate accurate tracking of the cache size.
        # For uncompressed pages read in memory, we allocate space based on their on-disk size rather than their in-memory size.
        # Conversely, for compressed pages read in memory, we allocate memory to the in-memory page size.
        # The assertion below verifies the same.
        cache_bytes_page_image = self.get_stat(wiredtiger.stat.conn.cache_bytes_image)
        if self.compressor == "none":
            self.assertGreater(cache_bytes_page_image, _128MB)
        else:
            self.assertLess(cache_bytes_page_image, _128MB)
