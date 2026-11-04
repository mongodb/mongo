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

import time
import wttest
from wiredtiger import stat
from wtscenario import make_scenarios


class test_eviction11(wttest.WiredTigerTestCase):
    """Check the statistics that count how eviction refills end."""

    cache_mb = 20
    conn_config = f"cache_size={cache_mb}MB,eviction_target=50,eviction_trigger=95," \
        "statistics=(all)"
    uri = f"file:{__qualname__}"
    value = "a" * 1000
    timeout = 120

    # filled: the table is twice the cache size, so there is plenty to evict.
    # lap_complete: the table cannot be evicted and holds 70% of the cache, so eviction runs but
    # finds nothing to queue.
    scenarios = make_scenarios([
        ("filled", dict(cache_resident=False, nrows=40 * 1024,
            counter=stat.conn.eviction_server_refill_end_filled)),
        ("lap_complete", dict(cache_resident=True, nrows=13 * 1024,
            counter=stat.conn.eviction_server_refill_end_lap_complete)),
    ])

    def scan(self):
        with wttest.open_cursor(self.session, self.uri) as cursor:
            self.assertEqual(sum(1 for _ in cursor), self.nrows)

    def test_refill_end(self):
        # Write the table with a large cache, then reopen with the small cache.
        self.conn.reconfigure("cache_size=200MB")
        self.session.create(self.uri,
            f"key_format=i,value_format=S,cache_resident={str(self.cache_resident).lower()}")
        with wttest.open_cursor(self.session, self.uri) as cursor:
            for i in range(self.nrows):
                cursor[i] = self.value
        self.session.checkpoint()
        self.reopen_conn()

        self.scan()
        if self.cache_resident:
            inuse = self.get_stat(stat.conn.cache_bytes_inuse)
            self.assertGreater(inuse, self.cache_mb * 1024 * 1024 * 0.5,
                "cache usage is below the eviction target")

        # Keep reading the table so eviction keeps running.
        deadline = time.time() + self.timeout
        while self.get_stat(self.counter) == 0:
            self.assertLess(time.time(), deadline, "no refill ended this way")
            self.scan()
            time.sleep(0.1)


if __name__ == "__main__":
    wttest.run()
