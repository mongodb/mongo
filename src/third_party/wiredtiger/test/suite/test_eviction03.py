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

import re
from eviction_util import eviction_util
from statistics import mean
from suite_subprocess import suite_subprocess
import wiredtiger

# test_eviction03.py
# Verify the disk footprint is reduced after eviction has removed obsolete time window information.
class test_eviction03(eviction_util, suite_subprocess):

    def verify_dump_pages(self, uri):
        outfilename = f'dump_{uri}.out'
        errfilename = f'dump_{uri}.err'
        self.runWt(['verify', '-d', 'dump_pages', uri], outfilename=outfilename, errfilename=errfilename)
        return outfilename

    def derive_avg_disk_footprint(self, filename):
        # Filename contains the output from verfify dump_pages which contains the space taken on
        # disk by each internal and leaf pages.
        file = open(filename, 'r')
        res = re.findall(r'dsk_mem_size: (\d+)', file.read(), re.DOTALL)
        file.close()

        # Convert all elements to integers.
        res = list(map(int, res))
        # Derive the average.
        avg = int(mean(res))

        return avg

    def test_eviction03(self):
        if not wiredtiger.diagnostic_build():
            self.skipTest('requires a diagnostic build as the test uses verify -d dump_pages')

        create_params = 'key_format=i,value_format=S'
        nrows = 10000
        ntables = 3
        uri_prefix = 'table:eviction03'
        value = 'k' * 1024

        # Create tables.
        uris = []
        for i in range(ntables):
            uri = f"{uri_prefix}_{i}"
            uris.append(uri)
            self.session.create(uri, create_params)

        # Pin oldest timestamp 1.
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(1)}')

        # Populate tables.
        for i in range(ntables):
            uri = f"{uri_prefix}_{i}"
            self.populate(uri, 0, nrows, value)

        # Bump the stable timestamp to make everything stable.
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(nrows + 1)}')

        # Close the connection, this will checkpoint the data and write everything to disk.
        self.close_conn()

        # Dump the data using the wt utlity and derive the average disk footprint. Store the values
        # to compare them later, after the cleanup.
        avg_disk_footprint_values = []
        for i in range(ntables):
            uri = f"{uri_prefix}_{i}"
            # Dump all pages using the verify command.
            filename = self.verify_dump_pages(uri)
            # Derive the average disk footprint.
            avg_disk_footprint = self.derive_avg_disk_footprint(filename)
            avg_disk_footprint_values.append(avg_disk_footprint)

        # The new connection is configured in a way to maximise the number of pages that can be
        # cleaned up.
        self.reopen_conn(config="heuristic_controls=[eviction_obsolete_tw_pages_dirty_max=10000]")

        # Update the oldest timestamp to make all the time window information obsolete.
        self.conn.set_timestamp(f'oldest_timestamp={self.timestamp_str(nrows + 1)}')

        # Scan all files and perform eviction to cleanup the obsolete time window information.
        for i in range(ntables):
            uri = f"{uri_prefix}_{i}"
            self.evict_cursor_tw_cleanup(uri, nrows)

        # Re-open the connection.
        self.reopen_conn()

        # Compare the new disk footprints now the time window information has been removed.
        for i in range(ntables):
            uri = f"{uri_prefix}_{i}"
            filename = self.verify_dump_pages(uri)
            avg_disk_footprint = self.derive_avg_disk_footprint(filename)
            assert avg_disk_footprint_values[i] > avg_disk_footprint, f"The average size on disk after cleanup has not reduced! {avg_disk_footprint_values[i]} > {avg_disk_footprint}"
