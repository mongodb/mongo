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

import wttest
from wiredtiger import stat


class test_eviction10(wttest.WiredTigerTestCase):
    """Cache pressure evicts pages from an ordinary file but never from a new cache-resident file."""

    conn_config = "cache_size=10MB"
    resident_uri = f"file:{__qualname__}_resident"
    ordinary_uri = f"file:{__qualname__}_ordinary"
    value = "a" * 1000

    def evicted_pages(self, uri):
        """Return the total number of clean and dirty pages evicted from the file."""
        clean = self.get_stat(stat.dsrc.cache_eviction_clean, uri)
        dirty = self.get_stat(stat.dsrc.cache_eviction_dirty, uri)
        return clean + dirty

    def write_rows(self, uri, nrows):
        """Write nrows to the file."""
        with wttest.open_cursor(self.session, uri) as cursor:
            for i in range(nrows):
                cursor[i] = self.value

    def test_cache_pressure_skips_new_cache_resident_file(self):
        self.session.create(
            self.resident_uri, "key_format=i,value_format=S,cache_resident=true"
        )
        self.write_rows(self.resident_uri, 100)

        # Write several times the cache size so eviction has to run.
        self.session.create(self.ordinary_uri, "key_format=i,value_format=S")
        self.write_rows(self.ordinary_uri, 50000)

        self.assertGreater(self.evicted_pages(self.ordinary_uri), 0)
        self.assertEqual(self.evicted_pages(self.resident_uri), 0)


if __name__ == "__main__":
    wttest.run()
