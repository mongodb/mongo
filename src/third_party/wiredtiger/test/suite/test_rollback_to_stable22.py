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

from test_rollback_to_stable01 import test_rollback_to_stable_base
from wtdataset import SimpleDataSet

# test_rollback_to_stable22
# Test history store operations conflicting with rollback to stable. We're trying to trigger a
# history store eviction concurrently to a rollback to stable call. We'll do this by limiting
# the cache size to 100MB and performing 100MB worth of inserts while periodically calling rollback
# to stable.
#
# The machinery this tests is all underneath the access-method-specific parts of RTS (and the
# history store itself is always row-store) so it doesn't seem necessary or worthwhile to run
# this explicitly on VLCS or FLCS.
class test_rollback_to_stable22(test_rollback_to_stable_base):
    conn_config = 'cache_size=100MB'
    session_config = 'isolation=snapshot'
    prepare = False

    def test_rollback_to_stable(self):
        nrows = 1000
        nds = 10

        # Create a few tables and populate them with some initial data.
        #
        # Our way of preventing history store operations from interfering with rollback to stable's
        # transaction check is by draining active evictions from each open dhandle.
        #
        # It's important that we have a few different tables to work with so that it's
        # representative of a real situation. But also don't make the number too high relative to
        # the number of updates or we may not have history for any of the tables.
        ds_list = list()
        for i in range(0, nds):
            uri = 'table:rollback_to_stable22_{}'.format(i)
            ds = SimpleDataSet(
                self, uri, 0, key_format='i', value_format='S', config='log=(enabled=false)')
            ds.populate()
            ds_list.append(ds)
        self.assertEqual(len(ds_list), nds)

        # 100 bytes of data are being inserted into 1000 rows.
        # This happens 1000 iterations.
        # Overall, that's 100MB of data which is guaranteed to kick start eviction.
        for i in range(1, 1000):
            # Generate a value, timestamp and table based off the index.
            value = str(i)[0] * 100
            ts = i * 10
            ds = ds_list[i % nds]

            # Perform updates.
            self.large_updates(ds.uri, value, ds, nrows, False, ts)

            # Every hundred updates, let's run rollback to stable. This is likely to happen during
            # a history store eviction at least once.
            if i % 100 == 0:
                # Put the timestamp backwards so we can rollback the updates we just did.
                stable_ts = (i - 1) * 10
                self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(stable_ts))
                self.conn.rollback_to_stable()
