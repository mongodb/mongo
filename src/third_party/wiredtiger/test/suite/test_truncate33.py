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

import re
import wttest
from suite_subprocess import suite_subprocess

# Test that an internal page whose children are all fast-truncated records the truncate
# stop point in its address aggregate. The aggregate written in a child's deleted-address
# cell describes the child as it was before the truncate, so a parent built by merging
# those cells unchanged claims its records never stop.
class test_truncate33(wttest.WiredTigerTestCase, suite_subprocess):
    conn_config = 'cache_size=50MB,statistics=(all)'
    uri = 'table:test_truncate33'

    # Small pages give a three-level tree, roughly 200 leaves under 10 internal pages, so
    # the truncated range covers every child of several internal pages with room to spare.
    create_cfg = ('key_format=i,value_format=S,allocation_size=512,leaf_page_max=512,'
                  'internal_page_max=512,memory_page_max=4096')

    nrows = 1000
    value = 'a' * 50
    trunc_start = 100
    trunc_stop = 900
    truncate_ts = 20

    def evict_all(self):
        with (
            wttest.open_cursor(
                self.session, self.uri, config='debug=(release_evict)'
            ) as evict_cursor,
            self.transaction(rollback=True),
        ):
            for key in range(1, self.nrows + 1):
                evict_cursor.set_key(key)
                evict_cursor.search()
                evict_cursor.reset()

    def populate(self):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        self.session.create(self.uri, self.create_cfg)

        with (
            wttest.open_cursor(self.session, self.uri) as cursor,
            self.transaction(commit_timestamp=10),
        ):
            for key in range(1, self.nrows + 1):
                cursor[key] = self.value

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        # Fast-truncate only applies to children that are already on disk.
        self.evict_all()

    def fast_truncate(self):
        with (
            wttest.open_cursor(self.session, self.uri) as start_cursor,
            wttest.open_cursor(self.session, self.uri) as stop_cursor,
            self.transaction(commit_timestamp=self.truncate_ts),
        ):
            start_cursor.set_key(self.trunc_start)
            stop_cursor.set_key(self.trunc_stop)
            self.session.truncate(None, start_cursor, stop_cursor, None)

    def internal_page_timestamps(self, dumpfile):
        """Collect stop timestamps of every internal page in a dump_address run."""
        stops = []
        durable_stops = []
        for line in open(dumpfile).readlines():
            if 'row-store internal' not in line:
                continue
            durable_match = re.search(r'newest_durable: \(\d+, \d+\)/\((\d+), (\d+)\)', line)
            match = re.search(r'newest_stop: \((\d+), (\d+)\)', line)
            if durable_match:
                durable_stops.append((int(durable_match.group(1)) << 32) +
                    int(durable_match.group(2)))
            if match:
                stops.append((int(match.group(1)) << 32) + int(match.group(2)))
        return stops, durable_stops

    def test_truncate_internal_page_aggregate(self):
        self.populate()
        self.fast_truncate()

        # Leave oldest behind the truncate so the deletions are not globally visible: the
        # children are written as deleted-address cells rather than being discarded.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.truncate_ts))
        self.session.checkpoint()

        self.runWt(['verify', '-d', 'dump_address', self.uri, '-d'], outfilename='dump.out')

        stops, durable_stops = self.internal_page_timestamps('dump.out')
        self.assertGreater(len(stops), 1, 'expected a tree with more than one internal page')
        self.assertIn(self.truncate_ts, stops,
            'no internal page aggregate records the truncate as its stop point')
        self.assertIn(self.truncate_ts, durable_stops,
            'no internal page aggregate records the truncate as its durable stop point')

if __name__ == '__main__':
    wttest.run()
